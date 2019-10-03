#pragma once

#include <cstddef>

class File;

using std::shared_ptr;

struct FileDescriptor {
  size_t location_index;
  shared_ptr<File> file;
  int access_mode;
  bool cloexec;

  FileDescriptor() {}

  FileDescriptor(size_t location_index, shared_ptr<File> file, int access_mode, bool cloexec) :
      location_index(location_index),
      file(file),
      access_mode(access_mode),
      cloexec(cloexec) {}
};
