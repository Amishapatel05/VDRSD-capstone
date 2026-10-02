// P1 self-check: write/read/overwrite/reopen/OOB. Run: ./block_store_demo (asserts abort on failure).
#include <cassert>
#include <cstdio>
#include <iostream>

#include "block_store.h"

int main() {
  const char* path = "/tmp/vdr_demo.blk";
  std::remove(path);
  std::string err;
  char w[BlockStore::kBlock] = {}, r[BlockStore::kBlock] = {};

  BlockStore s;
  assert(s.Open(path, 8, &err));
  for (size_t i = 0; i < sizeof w; ++i) w[i] = static_cast<char>(i & 0xFF);
  assert(s.Write(3, w, &err));
  assert(s.Fsync(&err));
  assert(s.Read(3, r, &err));
  assert(std::string(r, sizeof r) == std::string(w, sizeof w));

  w[0] = 'Z';  // overwrite + reopen persistence
  assert(s.Write(3, w, &err) && s.Fsync(&err));
  s.Close();
  BlockStore s2;
  assert(s2.Open(path, 8, &err) && s2.Read(3, r, &err) && r[0] == 'Z');

  assert(!s2.Read(8, r, &err) && !s2.Write(8, w, &err));  // OOB guards
  std::remove(path);
  std::cout << "block_store_demo: OK\n";
  return 0;
}
