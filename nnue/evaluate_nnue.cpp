/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2020 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Code for calculating NNUE evaluation function

#include <fstream>
#include <iostream>
#include <set>

#include "../evaluate.h"
#include "../position.h"
#include "../misc.h"
#include "../uci.h"

#include "evaluate_nnue.h"

namespace Eval::NNUE {

  uint32_t kpp_board_index[PIECE_NB][COLOR_NB] = {
   // convention: W - us, B - them
   // viewed from other side, W and B are reversed
      { PS_NONE,     PS_NONE     },
      { PS_W_PAWN,   PS_B_PAWN   },
      { PS_W_KNIGHT, PS_B_KNIGHT },
      { PS_W_BISHOP, PS_B_BISHOP },
      { PS_W_ROOK,   PS_B_ROOK   },
      { PS_W_QUEEN,  PS_B_QUEEN  },
      { PS_W_KING,   PS_B_KING   },
      { PS_NONE,     PS_NONE     },
      { PS_NONE,     PS_NONE     },
      { PS_B_PAWN,   PS_W_PAWN   },
      { PS_B_KNIGHT, PS_W_KNIGHT },
      { PS_B_BISHOP, PS_W_BISHOP },
      { PS_B_ROOK,   PS_W_ROOK   },
      { PS_B_QUEEN,  PS_W_QUEEN  },
      { PS_B_KING,   PS_W_KING   },
      { PS_NONE,     PS_NONE     }
  };

  // Input feature converter
  AlignedPtr<FeatureTransformer> feature_transformer;

  // Evaluation function
  AlignedPtr<Network> network;

  // Evaluation function file name
  std::string fileName;

  // Saved evaluation function file name
  std::string savedfileName = "nn.bin";

  // Get a string that represents the structure of the evaluation function
  std::string GetArchitectureString() {
    return "Features=" + FeatureTransformer::GetStructureString() +
      ",Network=" + Network::GetStructureString();
  }

  namespace Detail {

  // Initialize the evaluation function parameters
  template <typename T>
  void Initialize(AlignedPtr<T>& pointer) {

    pointer.reset(reinterpret_cast<T*>(std_aligned_alloc(alignof(T), sizeof(T))));
    std::memset(pointer.get(), 0, sizeof(T));
  }

  // Read evaluation function parameters
  template <typename T>
  bool ReadParameters(std::istream& stream, const AlignedPtr<T>& pointer) {

    std::uint32_t header;
    header = read_little_endian<std::uint32_t>(stream);
    if (!stream || header != T::GetHashValue()) return false;
    return pointer->ReadParameters(stream);
  }

  // write evaluation function parameters
  template <typename T>
  bool WriteParameters(std::ostream& stream, const AlignedPtr<T>& pointer) {
    constexpr std::uint32_t header = T::GetHashValue();
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    return pointer->WriteParameters(stream);
  }

  }  // namespace Detail

  // Initialize the evaluation function parameters
  void Initialize() {

    Detail::Initialize(feature_transformer);
    Detail::Initialize(network);
  }

  // Read network header
  bool ReadHeader(std::istream& stream, std::uint32_t* hash_value, std::string* architecture)
  {
    std::uint32_t version, size;

    version     = read_little_endian<std::uint32_t>(stream);
    *hash_value = read_little_endian<std::uint32_t>(stream);
    size        = read_little_endian<std::uint32_t>(stream);
    if (!stream || version != kVersion) return false;
    architecture->resize(size);
    stream.read(&(*architecture)[0], size);
    return !stream.fail();
  }

  // write the header
  bool WriteHeader(std::ostream& stream,
    std::uint32_t hash_value, const std::string& architecture) {
    stream.write(reinterpret_cast<const char*>(&kVersion), sizeof(kVersion));
    stream.write(reinterpret_cast<const char*>(&hash_value), sizeof(hash_value));
    const std::uint32_t size = static_cast<std::uint32_t>(architecture.size());
    stream.write(reinterpret_cast<const char*>(&size), sizeof(size));
    stream.write(architecture.data(), size);
    return !stream.fail();
  }

  // Read network parameters
  bool ReadParameters(std::istream& stream) {

    std::uint32_t hash_value;
    std::string architecture;
    if (!ReadHeader(stream, &hash_value, &architecture)) return false;
    if (hash_value != kHashValue) return false;
    if (!Detail::ReadParameters(stream, feature_transformer)) return false;
    if (!Detail::ReadParameters(stream, network)) return false;
    return stream && stream.peek() == std::ios::traits_type::eof();
  }

  // write evaluation function parameters
  bool WriteParameters(std::ostream& stream) {
    if (!WriteHeader(stream, kHashValue, GetArchitectureString())) return false;
    if (!Detail::WriteParameters(stream, feature_transformer)) return false;
    if (!Detail::WriteParameters(stream, network)) return false;
    return !stream.fail();
  }

  // Proceed with the difference calculation if possible
  static void UpdateAccumulatorIfPossible(const Position& pos) {

    feature_transformer->UpdateAccumulatorIfPossible(pos);
  }

  // Calculate the evaluation value
  static Value ComputeScore(const Position& pos, bool refresh) {

    auto& accumulator = pos.state()->accumulator;
    if (!refresh && accumulator.computed_score) {
      return accumulator.score;
    }

    alignas(kCacheLineSize) TransformedFeatureType
        transformed_features[FeatureTransformer::kBufferSize];
    feature_transformer->Transform(pos, transformed_features, refresh);
    alignas(kCacheLineSize) char buffer[Network::kBufferSize];
    const auto output = network->Propagate(transformed_features, buffer);

    auto score = static_cast<Value>(output[0] / FV_SCALE);

    accumulator.score = score;
    accumulator.computed_score = true;
    return accumulator.score;
  }

  // Load the evaluation function file
  bool load_eval_file(const std::string& evalFile) {

    Initialize();

    if (Options["SkipLoadingEval"])
    {
      std::cout << "info string SkipLoadingEval set to true, Net not loaded!" << std::endl;
      return true;
    }

    fileName = evalFile;

    std::ifstream stream(evalFile, std::ios::binary);

    const bool result = ReadParameters(stream);

    return result;
  }

  // Evaluation function. Perform differential calculation.
  Value evaluate(const Position& pos) {
    return ComputeScore(pos, false);
  }

  // Evaluation function. Perform full calculation.
  Value compute_eval(const Position& pos) {
    return ComputeScore(pos, true);
  }

  // Proceed with the difference calculation if possible
  void update_eval(const Position& pos) {
    UpdateAccumulatorIfPossible(pos);
  }

} // namespace Eval::NNUE
