/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

#pragma once

#include "import/splat_parsing.h"

namespace import::ply {
/**
 * `ply` data encoding.
 */
enum class PlyFormat { Invalid, ASCII, BinaryBigEndian, BinaryLittleEndian };

/**
 * Type and location within file of a property.
 */
struct PropertyDesc {
  uint64_t offset = 0;
  PropertyFormat type = PropertyFormat::Unknown;
};

/**
 * Parser for `.ply` 3DGS assets.
 *
 * @see https://gamma.cs.unc.edu/POWERPLANT/papers/ply.pdf
 */
class SplatParserPly final : public ISplatParser {
 public:
  // Stream-based methods for large files
  bool parse_header(std::istream& stream);
  bool parse_metadata(std::istream& stream, Metadata& metadata);
  bool parse_data(std::istream& stream, ParseSplatFn parse_splat);

  // Buffer-based methods (legacy/small files)
  //~ Begin ISplatParser Interface
  SPLAT_EXPORT_API virtual bool parse_metadata(
      std::span<const uint8_t> ply_buffer, Metadata& metadata) override;
  SPLAT_EXPORT_API virtual bool parse_data(ParseSplatFn parse_splat);
  //~ End ISplatParser Interface

  bool parse_header();
  bool add_property(Property property, PropertyFormat type);

 public:
  PlyFormat format = PlyFormat::Invalid;
  std::unordered_map<Property, PropertyDesc> layout;
  
  // 4DGS Extensions
  struct BankPropertyDesc {
    int32_t bank_index;
    uint64_t offset;
    PropertyFormat type;
  };
  std::vector<BankPropertyDesc> xyz_banks;
  std::vector<BankPropertyDesc> rot_banks;
  std::vector<BankPropertyDesc> dc_banks;
  std::vector<BankPropertyDesc> sh_rest;
  
  int32_t total_frames = 0;
  int32_t xyz_stride = 0;
  int32_t rot_stride = 0;
  int32_t dc_stride = 0;

  size_t num_splats = 0;
  size_t splat_size = 0;
  std::span<const uint8_t> buffer;
};
}  // namespace import::ply

