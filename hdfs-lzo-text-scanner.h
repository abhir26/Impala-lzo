// Copyright (c) 2012 Cloudera, Inc. All rights reserved.

#ifndef IMPALA_LZO_TEXT_SCANNER_H
#define IMPALA_LZO_TEXT_SCANNER_H

#include "lzo-header.h"
#include <boost/thread/locks.hpp>
#include "common/version.h"
#include "exec/hdfs-text-scanner.h"
#include "runtime/string-buffer.h"

// This provides support for reading files compressed with lzop.
// The file consists of a header and compressed blocks preceeded
// by their compressed and uncompressed block sizes.
//
// The following is a pseudo-BNF grammar for LZOFile. Comments are prefixed
// with dashes:
//
// lzofile ::=
//   <file-header>
//   <compressed-block>+
//
// compressed-block ::=
//   <uncompressed-size>
//   <compressed-size>
//   <uncompressed-checksums>
//   <compressed-checksums>
//   <compressed-data>
//
// file-header ::=  -- most of this information is not used.
//   <magic>
//   <version>
//   <lib-version>
//   [<version-needed>] -- present for all modern files.
//   <method>
//   <flags>
//   <mode>
//   <mtime>
//   <file-name>
//   <header-checksum>
//   <extra-field> -- presence indicated in flags, not currently used.
//
// <compressed-checksums> ::=
//   [alder-checksum | crc-checksum]
//
// <uncompressed-checksums> ::=
//   [alder-checksum | crc-checksum]
//
// <file-name> ::=
//   <length> -- one byte
//   <name>
//
namespace impala {

class ScannerContext;
class HdfsLzoTextScanner;

// HdfsScanner implementation that reads LZOP formatted text files.
// The format of the data, after decompression, is the same as HdfsText files.
// Records can span compresed blocks.
//
// An optional, but highly recommended, index file may exist in the same directory.
// This file is generated by running: com.hadoop.compression.lzo.DistributedLzoIndexer.
// The file contains the offsets to the start of each compressed block.
// This is used to find the beginning of a split and to skip over a bad block and
// find the next block.
// If there is no index file then the file is non-splittble. A single scan range
// will be issued for the whole file and no error recovery is done.


// Used to verify that this library was built against the expected Impala version when the
// library is loaded via dlopen.
// The function called resides in common/version.h.
extern "C" const char* GetImpalaBuildVersion() { return GetDaemonBuildVersion(); }

// The two functions below are wrappers for calling methods of HdfsLzoTextScanner
// when the library is loaded via dlopen.
// This function is a wrapper for the HdfsLzoTextScanner creator.  The caller is expected
// to call delete on it.
// scan_node -- scan node that is creating this scanner.
// state -- runtime state for this scanner.
extern "C" HdfsLzoTextScanner* GetLzoTextScanner(
    HdfsScanNodeBase* scan_node, RuntimeState* state);

// This function is a wrapper for HdfsLzoTextScanner::IssueInitialRanges.
// scan_node -- scan node for this scan
// files -- files that are to be scanned.
extern "C" Status LzoIssueInitialRangesImpl(
    HdfsScanNodeBase* scan_node, const std::vector<HdfsFileDesc*>& files);

class HdfsLzoTextScanner : public HdfsTextScanner {
 public:
  HdfsLzoTextScanner(HdfsScanNodeBase* scan_node, RuntimeState* state);
  virtual ~HdfsLzoTextScanner();

  // Determines whether this scanner is processing an initial scan range for which it
  // should only parse the file header and index file (if any). For non-initial scan
  // ranges, stream_ is positioned to the first byte that contains data.
  // Sets 'only_parsing_header_' and 'header_'.  Sets 'eos_' to true if this scan range
  // contains no tuples for which this scanner is responsible.
  virtual Status Open(ScannerContext* context);

  // If 'only_parsing_header_' is true, processes the header and index file, issues new
  // scan ranges for the data and sets 'eos_' to true. Registers the header as scan range
  // metadata in the parent scan node.
  // Otherwise, calls the parent's GetNextInternal().
  virtual Status GetNextInternal(RowBatch* row_batch);

  // Attaches 'block_buffer_pool_' to 'row_batch'. If 'row_batch' is nullptr,
  // then 'block_buffer_pool_' is freed instead. Calls the parent's Close().
  virtual void Close(RowBatch* row_batch);

  // Issue the initial scan ranges for all lzo-text files. This reads the
  // file headers and then the reset of the file data will be issued from
  // ProcessScanRange().
  static Status LzoIssueInitialRangesImpl(
      HdfsScanNodeBase* scan_node, const std::vector<HdfsFileDesc*>& files);

 private:
  enum LzoChecksum {
    CHECK_NONE,
    CHECK_CRC32,
    CHECK_ADLER
  };

  // Block size in bytes used by LZOP. The compressed blocks will be no bigger than this.
  const static int MAX_BLOCK_COMPRESSED_SIZE = (256 * 1024);

  // This is the fixed size of the header. It can have up to 255 bytes of
  // file name in it as well.
  const static int MIN_HEADER_SIZE = 32;

  // An over estimate of how big the header could be.  There is a path name
  // and an option seciton.
  const static int HEADER_SIZE = 300;

  // Header informatation, shared by all scanners on this file.
  struct LzoFileHeader {
    LzoChecksum input_checksum_type_;
    LzoChecksum output_checksum_type_;

    uint32_t header_size_;

    // Offsets to compressed blocks.
    std::vector<int64_t> offsets;
  };

  // Pointer to shared header information.
  LzoFileHeader* header_;

  // Fills the byte buffer by reading and decompressing blocks.
  virtual Status FillByteBuffer(MemPool* pool, bool* eosr, int num_bytes = 0);

  // Read header data and validate header.
  Status ReadHeader();

  // Read the index file and set up the header.offsets.
  Status ReadIndexFile();

  // Checksum data.
  Status Checksum(LzoChecksum type,
    const std::string& source, int expected_checksum, uint8_t* buffer, int length);

  // Adjust the context_ to the first block at or after the current context offset.
  // *found returns if a starting block was found.
  Status FindFirstBlock(bool* found);

  // Issue the full file ranges after reading the headers.
  Status IssueFileRanges(const char* filename);

  // Read a data block.
  // sets: byte_buffer_ptr_, byte_buffer_read_size_ and eos_read_.
  // Data will be in a mempool allocated buffer or in the disk I/O context memory
  // if the data was not compressed.
  // Attaches decompression buffers from previous calls that might still be referenced
  // by returned batches to 'pool'.
  Status ReadAndDecompressData(MemPool* pool);

  // Read compress data and recover from errosr.
  // Attaches decompression buffers from previous calls that might still be referenced
  // by returned batches to 'pool'.
  Status ReadData(MemPool* pool);

  // Callback for stream_ to determine how much to read past the scan range.
  static int MaxBlockCompressedSize(int64_t file_offset) {
    return MAX_BLOCK_COMPRESSED_SIZE;
  }

  // Pool for allocating the block_buffer_.
  boost::scoped_ptr<MemPool> block_buffer_pool_;

  // Buffer to hold decompressed data.
  uint8_t* block_buffer_;

  // Allocated length of the block_buffer_
  int32_t block_buffer_len_;

  // Next byte to be returned from the buffer holding decompressed data blocks.
  uint8_t* block_buffer_ptr_;

  // Bytes remaining in the block_buffer.
  int bytes_remaining_;

  // True if the end of scan has been read.
  bool eos_read_;

  // This is set when the scanner object is constructed.  Currently always true.
  // HDFS checksums the blocks from the disk to the client, so this is redundent.
  bool disable_checksum_;
};

}
#endif
