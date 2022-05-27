#include <Zydis/Zydis.h>
#include <cstdio>
#include <vector>
#include <fstream>
#include <Windows.h>

// instructions that need to be "fixed"
struct relative_instruction {
  // virtual offset of this instruction
  std::size_t new_virtual_offset;

  // relative offset to the target
  std::int32_t target_delta;
};

class chum_parser {
public:
  chum_parser(char const* const file_path) {
    // initialize the decoder
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
      printf("[!] Failed to initialize Zydis decoder.\n");
      return;
    }

    // initialize the formatter
    if (ZYAN_FAILED(ZydisFormatterInit(&formatter_, ZYDIS_FORMATTER_STYLE_INTEL))) {
      printf("[!] Failed to initialize Zydis formatter.\n");
      return;
    }

    ZydisFormatterSetProperty(&formatter_, ZYDIS_FORMATTER_PROP_FORCE_RELATIVE_BRANCHES, true);
    ZydisFormatterSetProperty(&formatter_, ZYDIS_FORMATTER_PROP_FORCE_RELATIVE_RIPREL,   true);
    ZydisFormatterSetProperty(&formatter_, ZYDIS_FORMATTER_PROP_PRINT_BRANCH_SIZE,       true);

    file_buffer_ = read_file_to_buffer(file_path);
    if (file_buffer_.empty()) {
      printf("[!] Failed to read file.\n");
      return;
    }

    dos_header_ = reinterpret_cast<PIMAGE_DOS_HEADER>(&file_buffer_[0]);
    nt_header_  = reinterpret_cast<PIMAGE_NT_HEADERS>(&file_buffer_[dos_header_->e_lfanew]);
    sections_   = reinterpret_cast<PIMAGE_SECTION_HEADER>(nt_header_ + 1);

    // the exception directory (aka the .pdata section) contains an array of functions
    auto const& exception_dir = nt_header_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    runtime_funcs_ = reinterpret_cast<PRUNTIME_FUNCTION>(
      &file_buffer_[rva_to_file_offset(exception_dir.VirtualAddress)]);
    runtime_funcs_count_ = exception_dir.Size / sizeof(RUNTIME_FUNCTION);

    if (!parse())
      printf("[!] Failed to parse binary.\n");
  }

  // write the new binary to memory
  bool write() {
    return true;
  }

  // memory where code will reside (X)
  void add_code_region(void* const virtual_address, std::size_t const size) {
    code_regions_.push_back({ static_cast<std::uint8_t*>(virtual_address), size });
  }

  // memory where data will reside (RW)
  void add_data_region(void* const virtual_address, std::size_t const size) {
    data_regions_.push_back({ static_cast<std::uint8_t*>(virtual_address), size });
  }

private:
  // the real "meat" of the parser
  bool parse() {
    // TODO: add external references to code regions that are not covered by
    //       exception directory.

    
    // disassemble every function and create a list of instructions that
    // will need to be fixed later on.
    for (std::size_t i = 0; i < runtime_funcs_count_; ++i) {
      auto const& runtime_func = runtime_funcs_[i];

      // virtual offset, file offset, and size of the current code region
      auto const region_virt_offset = runtime_func.BeginAddress;
      auto const region_file_offset = rva_to_file_offset(runtime_func.BeginAddress);
      auto const region_size        = (runtime_func.EndAddress - runtime_func.BeginAddress);

      ZydisDecodedInstruction decoded_instruction;
      ZydisDecodedOperand decoded_operands[ZYDIS_MAX_OPERAND_COUNT_VISIBLE];

      // disassemble every instruction in this region
      for (std::size_t instruction_offset = 0;
           instruction_offset < region_size;
           instruction_offset += decoded_instruction.length) {
        // pointer to the current instruction in the binary blob
        auto const buffer_curr_instruction = &file_buffer_[region_file_offset + instruction_offset];
        auto const remaining_size = (region_size - instruction_offset);

        // decode the current instruction
        auto const status = ZydisDecoderDecodeFull(&decoder_, buffer_curr_instruction,
          remaining_size, &decoded_instruction, decoded_operands,
          ZYDIS_MAX_OPERAND_COUNT_VISIBLE, ZYDIS_DFLAG_VISIBLE_OPERANDS_ONLY);
        
        // this *really* shouldn't happen but it isn't a fatal error... just
        // ignore any possible remaining instructions in the region.
        if (ZYAN_FAILED(status)) {
          printf("[!] Failed to decode instruction! VA=0x%zX. Status=0x%X.\n",
            region_virt_offset + instruction_offset, status);
          break;
        }

        // we only need to fix relative instructions
        if (!(decoded_instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE))
          continue;

        // only one of the operands can be relative (i think?)
        for (std::size_t j = 0; j < decoded_instruction.operand_count_visible; ++j) {
          auto const& op = decoded_operands[j];

          // memory references
          if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            // sanity check
            if (op.mem.base  != ZYDIS_REGISTER_RIP  ||
                op.mem.index != ZYDIS_REGISTER_NONE ||
                op.mem.scale != 0 ||
               !op.mem.disp.has_displacement) {
              printf("[!] Memory operand isn't RIP-relative!\n");
              return false;
            }

            printf("[+] Memory operand displacement: %+zd.\n", op.mem.disp.value);
            break;
          }
          // relative CALLs, JMPs, etc
          else if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative) {
            printf("[+] Immediate operand value: %+zd.\n", op.imm.value.s);
            break;
          }
        }
      }
    }

    return true;
  }

  // read all the contents of a file and return the bytes in a vector
  static std::vector<std::uint8_t> read_file_to_buffer(char const* const path) {
    // open the file
    std::ifstream file(path, std::ios::binary);
    if (!file)
      return {};

    // get the size of the file
    file.seekg(0, file.end);
    std::vector<std::uint8_t> contents(file.tellg());
    file.seekg(0, file.beg);

    // read
    file.read((char*)contents.data(), contents.size());

    return contents;
  }

  // convert an RVA offset to a file offset
  std::size_t rva_to_file_offset(std::size_t const rva) const {
    for (std::size_t i = 0; i < nt_header_->FileHeader.NumberOfSections; ++i) {
      auto const& section = sections_[i];

      if (rva >= section.VirtualAddress && rva < (section.VirtualAddress + section.Misc.VirtualSize))
        return (rva - section.VirtualAddress) + section.PointerToRawData;
    }

    return 0;
  }

private:
  struct memory_region {
    std::uint8_t* virtual_address;
    std::size_t size;
  };

private:
  // zydis
  ZydisDecoder decoder_     = {};
  ZydisFormatter formatter_ = {};

  // raw binary blob of the PE file
  std::vector<std::uint8_t> file_buffer_ = {};

  // pointers into the file buffer
  PIMAGE_DOS_HEADER dos_header_   = nullptr;
  PIMAGE_NT_HEADERS nt_header_    = nullptr;
  PIMAGE_SECTION_HEADER sections_ = nullptr;

  // exception directory
  PRUNTIME_FUNCTION runtime_funcs_ = nullptr;
  std::size_t runtime_funcs_count_ = 0;

  // this is where the binary will be written to
  std::vector<memory_region> code_regions_ = {};
  std::vector<memory_region> data_regions_ = {};
};

int main() {
  chum_parser chum("./hello-world-x64.dll");

  // add 0x2000 bytes of executable memory and 0x2000 bytes of read-write memory
  chum.add_code_region(VirtualAlloc(nullptr, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE), 0x2000);
  chum.add_data_region(VirtualAlloc(nullptr, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),         0x2000);

  if (!chum.write())
    printf("[!] Failed to write binary to memory.\n");
}
