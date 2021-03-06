// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include <algorithm>
#include <iostream>

#include "proto/gen/db_operation.pb.h"
#include "rocksdb/file_system.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksdb/sst_file_writer.h"
#include "src/libfuzzer/libfuzzer_macro.h"

// Keys in SST file writer operations must be unique and in ascending order.
// For each DBOperation generated by the fuzzer, this function is called on
// it to deduplicate and sort the keys in the DBOperations.
protobuf_mutator::libfuzzer::PostProcessorRegistration<DBOperations> reg = {
    [](DBOperations* input, unsigned int /* seed */) {
      const rocksdb::Comparator* comparator = rocksdb::BytewiseComparator();
      auto ops = input->mutable_operations();
      std::sort(ops->begin(), ops->end(),
                [&comparator](const DBOperation& a, const DBOperation& b) {
                  return comparator->Compare(a.key(), b.key()) < 0;
                });

      auto last = std::unique(
          ops->begin(), ops->end(),
          [&comparator](const DBOperation& a, const DBOperation& b) {
            return comparator->Compare(a.key(), b.key()) == 0;
          });
      ops->erase(last, ops->end());
    }};

#define CHECK_OK(status)                         \
  if (!status.ok()) {                            \
    std::cerr << status.ToString() << std::endl; \
    abort();                                     \
  }

// Fuzzes DB operations as input, let SstFileWriter generate a SST file
// according to the operations, then let SstFileReader read the generated SST
// file to check its checksum.
DEFINE_PROTO_FUZZER(DBOperations& input) {
  if (input.operations().empty()) {
    return;
  }

  std::string sstfile;
  {
    auto fs = rocksdb::FileSystem::Default();
    std::string dir;
    rocksdb::IOOptions opt;
    rocksdb::IOStatus s = fs->GetTestDirectory(opt, &dir, nullptr);
    CHECK_OK(s);
    sstfile = dir + "/SstFileWriterFuzzer.sst";
  }

  // Generate sst file.
  rocksdb::Options options;
  rocksdb::EnvOptions env_options;
  rocksdb::SstFileWriter writer(env_options, options);
  rocksdb::Status s = writer.Open(sstfile);
  CHECK_OK(s);
  for (const DBOperation& op : input.operations()) {
    switch (op.type()) {
      case OpType::PUT:
        s = writer.Put(op.key(), op.value());
        CHECK_OK(s);
        break;
      case OpType::MERGE:
        s = writer.Merge(op.key(), op.value());
        CHECK_OK(s);
        break;
      case OpType::DELETE:
        s = writer.Delete(op.key());
        CHECK_OK(s);
        break;
      case OpType::DELETE_RANGE:
        s = writer.DeleteRange(op.key(), op.value());
        CHECK_OK(s);
        break;
      default:
        std::cerr << "Unsupported operation" << static_cast<int>(op.type());
        return;
    }
  }
  rocksdb::ExternalSstFileInfo info;
  s = writer.Finish(&info);
  CHECK_OK(s);

  // Verify checksum.
  rocksdb::SstFileReader reader(options);
  s = reader.Open(sstfile);
  CHECK_OK(s);
  s = reader.VerifyChecksum();
  CHECK_OK(s);

  // Delete sst file.
  remove(sstfile.c_str());
}
