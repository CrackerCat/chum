#include "chum.h"

int main() {
  chum::binary binary;

  //if (!binary.load("C:\\Users\\realj\\Desktop\\ntoskrnl (19041.1110).exe")) {
  //if (!binary.load("hello-world-x64.dll")) {
  if (!binary.load("hello-world-x64-min.dll")) {
    std::printf("[!] Failed to load binary.\n");
    return 0;
  }

  binary.print();
}

