// Copyright 2010-2016, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "converter/nbest_generator.h"

#include <memory>
#include <string>

#include "base/logging.h"
#include "base/port.h"
#include "base/system_util.h"
#include "config/config_handler.h"
#include "converter/connector.h"
#include "converter/immutable_converter.h"
#include "converter/segmenter.h"
#include "converter/segments.h"
#include "data_manager/data_manager_interface.h"
#include "data_manager/testing/mock_data_manager.h"
#include "dictionary/dictionary_impl.h"
#include "dictionary/dictionary_interface.h"
#include "dictionary/pos_group.h"
#include "dictionary/suffix_dictionary.h"
#include "dictionary/suppression_dictionary.h"
#include "dictionary/system/system_dictionary.h"
#include "dictionary/system/value_dictionary.h"
#include "dictionary/user_dictionary_stub.h"
#include "prediction/suggestion_filter.h"
#include "request/conversion_request.h"
#include "testing/base/public/googletest.h"
#include "testing/base/public/gunit.h"

using mozc::dictionary::DictionaryImpl;
using mozc::dictionary::DictionaryInterface;
using mozc::dictionary::POSMatcher;
using mozc::dictionary::PosGroup;
using mozc::dictionary::SuffixDictionary;
using mozc::dictionary::SuppressionDictionary;
using mozc::dictionary::SystemDictionary;
using mozc::dictionary::UserDictionaryStub;
using mozc::dictionary::ValueDictionary;

namespace mozc {
namespace {

class MockDataAndImmutableConverter {
 public:
  // Initializes data and immutable converter with given dictionaries.
  MockDataAndImmutableConverter() {
    data_manager_.reset(new testing::MockDataManager);

    pos_matcher_.Set(data_manager_->GetPOSMatcherData());

    suppression_dictionary_.reset(new SuppressionDictionary);
    CHECK(suppression_dictionary_.get());

    const char *dictionary_data = NULL;
    int dictionary_size = 0;
    data_manager_->GetSystemDictionaryData(&dictionary_data,
                                           &dictionary_size);
    SystemDictionary *sysdic =
        SystemDictionary::Builder(dictionary_data, dictionary_size).Build();
    dictionary_.reset(new DictionaryImpl(
        sysdic,  // DictionaryImpl takes the ownership
        new ValueDictionary(pos_matcher_, &sysdic->value_trie()),
        &user_dictionary_stub_,
        suppression_dictionary_.get(),
        &pos_matcher_));
    CHECK(dictionary_.get());

    StringPiece suffix_key_array_data, suffix_value_array_data;
    const uint32 *token_array;
    data_manager_->GetSuffixDictionaryData(&suffix_key_array_data,
                                           &suffix_value_array_data,
                                           &token_array);
    suffix_dictionary_.reset(new SuffixDictionary(suffix_key_array_data,
                                                  suffix_value_array_data,
                                                  token_array));
    CHECK(suffix_dictionary_.get());

    connector_.reset(Connector::CreateFromDataManager(*data_manager_));
    CHECK(connector_.get());

    segmenter_.reset(Segmenter::CreateFromDataManager(*data_manager_));
    CHECK(segmenter_.get());

    pos_group_.reset(new PosGroup(data_manager_->GetPosGroupData()));
    CHECK(pos_group_.get());

    {
      const char *data = NULL;
      size_t size = 0;
      data_manager_->GetSuggestionFilterData(&data, &size);
      suggestion_filter_.reset(new SuggestionFilter(data, size));
    }

    immutable_converter_.reset(new ImmutableConverterImpl(
        dictionary_.get(),
        suffix_dictionary_.get(),
        suppression_dictionary_.get(),
        connector_.get(),
        segmenter_.get(),
        &pos_matcher_,
        pos_group_.get(),
        suggestion_filter_.get()));
    CHECK(immutable_converter_.get());
  }

  ImmutableConverterImpl *GetConverter() {
    return immutable_converter_.get();
  }

  NBestGenerator *CreateNBestGenerator(const Lattice *lattice) {
    return new NBestGenerator(suppression_dictionary_.get(),
                              segmenter_.get(),
                              connector_.get(),
                              &pos_matcher_,
                              lattice,
                              suggestion_filter_.get(),
                              true);
  }

 private:
  std::unique_ptr<const DataManagerInterface> data_manager_;
  std::unique_ptr<const SuppressionDictionary> suppression_dictionary_;
  std::unique_ptr<const Connector> connector_;
  std::unique_ptr<const Segmenter> segmenter_;
  std::unique_ptr<const DictionaryInterface> suffix_dictionary_;
  std::unique_ptr<const DictionaryInterface> dictionary_;
  std::unique_ptr<const PosGroup> pos_group_;
  std::unique_ptr<const SuggestionFilter> suggestion_filter_;
  std::unique_ptr<ImmutableConverterImpl> immutable_converter_;
  UserDictionaryStub user_dictionary_stub_;
  dictionary::POSMatcher pos_matcher_;
};

}  // namespace

class NBestGeneratorTest : public ::testing::Test {
 protected:
  void GatherCandidates(
      size_t size, Segments::RequestType request_type,
      NBestGenerator *nbest, Segment *segment) const {
    while (segment->candidates_size() < size) {
      Segment::Candidate *candidate = segment->push_back_candidate();
      candidate->Init();

      if (!nbest->Next(segment->key(), candidate, request_type)) {
        segment->pop_back_candidate();
        break;
      }
    }
  }

  const Node *GetEndNode(const ImmutableConverterImpl &converter,
                         const Segments &segments, const Node &begin_node,
                         const std::vector<uint16> &group,
                         bool is_single_segment) {
    const Node *end_node = NULL;
    for (Node *node = begin_node.next; node->next != NULL; node = node->next) {
      end_node = node->next;
      if (converter.IsSegmentEndNode(
              segments, node, group, is_single_segment)) {
        break;
      }
    }
    return end_node;
  }
};

TEST_F(NBestGeneratorTest, MultiSegmentConnectionTest) {
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  ImmutableConverterImpl *converter = data_and_converter->GetConverter();

  Segments segments;
  segments.set_request_type(Segments::CONVERSION);
  {
    Segment *segment = segments.add_segment();
    segment->set_segment_type(Segment::FIXED_BOUNDARY);
    // "しんこう"
    segment->set_key("\xe3\x81\x97\xe3\x82\x93\xe3\x81\x93\xe3\x81\x86");

    segment = segments.add_segment();
    segment->set_segment_type(Segment::FREE);
    // "する"
    segment->set_key("\xe3\x81\x99\xe3\x82\x8b");
  }

  Lattice lattice;
  // "しんこうする"
  lattice.SetKey("\xe3\x81\x97\xe3\x82\x93\xe3\x81\x93"
                 "\xe3\x81\x86\xe3\x81\x99\xe3\x82\x8b");
  const ConversionRequest request;
  converter->MakeLattice(request, &segments, &lattice);

  std::vector<uint16> group;
  converter->MakeGroup(segments, &group);
  converter->Viterbi(segments, &lattice);

  std::unique_ptr<NBestGenerator> nbest_generator(
      data_and_converter->CreateNBestGenerator(&lattice));

  const bool kSingleSegment = false;  // For 'normal' conversion
  const Node *begin_node = lattice.bos_nodes();
  const Node *end_node = GetEndNode(
      *converter, segments, *begin_node, group, kSingleSegment);

  {
    nbest_generator->Reset(begin_node, end_node, NBestGenerator::STRICT);
    Segment result_segment;
    GatherCandidates(
        10, Segments::CONVERSION, nbest_generator.get(), &result_segment);
    // The top result is treated exceptionally and has no boundary check
    // in NBestGenerator.
    // The best route is calculated in ImmutalbeConverter with boundary check.
    // So, the top result should be inserted, but other candidates will be cut
    // due to boundary check between "する".
    ASSERT_EQ(1, result_segment.candidates_size());
    // "進行"
    EXPECT_EQ("\xe9\x80\xb2\xe8\xa1\x8c", result_segment.candidate(0).value);
  }

  {
    nbest_generator->Reset(begin_node, end_node, NBestGenerator::ONLY_MID);
    Segment result_segment;
    GatherCandidates(
        10, Segments::CONVERSION, nbest_generator.get(), &result_segment);
    ASSERT_EQ(3, result_segment.candidates_size());
    // "進行"
    EXPECT_EQ("\xe9\x80\xb2\xe8\xa1\x8c", result_segment.candidate(0).value);
    // "信仰"
    EXPECT_EQ("\xe4\xbf\xa1\xe4\xbb\xb0", result_segment.candidate(1).value);
    // "深耕"
    EXPECT_EQ("\xe6\xb7\xb1\xe8\x80\x95", result_segment.candidate(2).value);
  }
}

TEST_F(NBestGeneratorTest, SingleSegmentConnectionTest) {
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  ImmutableConverterImpl *converter = data_and_converter->GetConverter();

  Segments segments;
  segments.set_request_type(Segments::CONVERSION);
  // "わたしのなまえはなかのです"
  string kText = ("\xe3\x82\x8f\xe3\x81\x9f\xe3\x81\x97\xe3\x81\xae"
                  "\xe3\x81\xaa\xe3\x81\xbe\xe3\x81\x88\xe3\x81\xaf"
                  "\xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xae\xe3\x81\xa7"
                  "\xe3\x81\x99");
  {
    Segment *segment = segments.add_segment();
    segment->set_segment_type(Segment::FREE);
    segment->set_key(kText);
  }

  Lattice lattice;
  lattice.SetKey(kText);
  const ConversionRequest request;
  converter->MakeLattice(request, &segments, &lattice);

  std::vector<uint16> group;
  converter->MakeGroup(segments, &group);
  converter->Viterbi(segments, &lattice);

  std::unique_ptr<NBestGenerator> nbest_generator(
      data_and_converter->CreateNBestGenerator(&lattice));


  const bool kSingleSegment = true;  // For realtime conversion
  const Node *begin_node = lattice.bos_nodes();
  const Node *end_node = GetEndNode(
      *converter, segments, *begin_node, group, kSingleSegment);

  {
    nbest_generator->Reset(begin_node, end_node, NBestGenerator::STRICT);
    Segment result_segment;
    GatherCandidates(
        10, Segments::CONVERSION, nbest_generator.get(), &result_segment);
    // Top result should be inserted, but other candidates will be cut
    // due to boundary check.
    ASSERT_EQ(1, result_segment.candidates_size());
    // "私の名前は中ノです"
    EXPECT_EQ("\xe7\xa7\x81\xe3\x81\xae\xe5\x90\x8d\xe5\x89\x8d"
              "\xe3\x81\xaf\xe4\xb8\xad\xe3\x83\x8e\xe3\x81\xa7\xe3\x81\x99",
              result_segment.candidate(0).value);
  }
  {
    nbest_generator->Reset(begin_node, end_node, NBestGenerator::ONLY_EDGE);
    Segment result_segment;
    GatherCandidates(
        10, Segments::CONVERSION, nbest_generator.get(), &result_segment);
    // We can get several candidates.
    ASSERT_LT(1, result_segment.candidates_size());
    // "私の名前は中ノです"
    EXPECT_EQ("\xe7\xa7\x81\xe3\x81\xae\xe5\x90\x8d\xe5\x89\x8d"
              "\xe3\x81\xaf\xe4\xb8\xad\xe3\x83\x8e\xe3\x81\xa7\xe3\x81\x99",
              result_segment.candidate(0).value);
  }
}

TEST_F(NBestGeneratorTest, InnerSegmentBoundary) {
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  ImmutableConverterImpl *converter = data_and_converter->GetConverter();

  Segments segments;
  segments.set_request_type(Segments::PREDICTION);
  // "とうきょうかなごやにいきたい"
  const string kInput =
      "\xe3\x81\xa8\xe3\x81\x86\xe3\x81\x8d\xe3\x82\x87\xe3\x81\x86"
      "\xe3\x81\x8b\xe3\x81\xaa\xe3\x81\x94\xe3\x82\x84\xe3\x81\xab"
      "\xe3\x81\x84\xe3\x81\x8d\xe3\x81\x9f\xe3\x81\x84";
  {
    Segment *segment = segments.add_segment();
    segment->set_segment_type(Segment::FREE);
    segment->set_key(kInput);
  }

  Lattice lattice;
  lattice.SetKey(kInput);
  const ConversionRequest request;
  converter->MakeLattice(request, &segments, &lattice);

  std::vector<uint16> group;
  converter->MakeGroup(segments, &group);
  converter->Viterbi(segments, &lattice);

  std::unique_ptr<NBestGenerator> nbest_generator(
      data_and_converter->CreateNBestGenerator(&lattice));

  const bool kSingleSegment = true;  // For realtime conversion
  const Node *begin_node = lattice.bos_nodes();
  const Node *end_node = GetEndNode(
      *converter, segments, *begin_node, group, kSingleSegment);

  nbest_generator->Reset(begin_node, end_node, NBestGenerator::ONLY_EDGE);
  Segment result_segment;
  GatherCandidates(
      10, Segments::PREDICTION, nbest_generator.get(), &result_segment);
  ASSERT_LE(1, result_segment.candidates_size());

  const Segment::Candidate &top_cand = result_segment.candidate(0);
  EXPECT_EQ(kInput, top_cand.key);
  // "東京か名古屋に行きたい
  EXPECT_EQ("\xe6\x9d\xb1\xe4\xba\xac\xe3\x81\x8b\xe5\x90\x8d\xe5\x8f\xa4"
            "\xe5\xb1\x8b\xe3\x81\xab\xe8\xa1\x8c\xe3\x81\x8d\xe3\x81\x9f"
            "\xe3\x81\x84",
            top_cand.value);

  std::vector<StringPiece> keys, values, content_keys, content_values;
  for (Segment::Candidate::InnerSegmentIterator iter(&top_cand);
       !iter.Done(); iter.Next()) {
    keys.push_back(iter.GetKey());
    values.push_back(iter.GetValue());
    content_keys.push_back(iter.GetContentKey());
    content_values.push_back(iter.GetContentValue());
  }
  ASSERT_EQ(3, keys.size());
  ASSERT_EQ(3, values.size());
  ASSERT_EQ(3, content_keys.size());
  ASSERT_EQ(3, content_values.size());

  // Inner segment 0
  // "とうきょうか"
  EXPECT_EQ("\xe3\x81\xa8\xe3\x81\x86\xe3\x81\x8d\xe3\x82\x87\xe3\x81\x86"
            "\xe3\x81\x8b", keys[0]);
  // "東京か"
  EXPECT_EQ("\xe6\x9d\xb1\xe4\xba\xac\xe3\x81\x8b", values[0]);
  // "とうきょう"
  EXPECT_EQ("\xe3\x81\xa8\xe3\x81\x86\xe3\x81\x8d\xe3\x82\x87\xe3\x81\x86",
            content_keys[0]);
  // "東京"
  EXPECT_EQ("\xe6\x9d\xb1\xe4\xba\xac", content_values[0]);

  // Inner segment 1
  // "なごやに"
  EXPECT_EQ("\xe3\x81\xaa\xe3\x81\x94\xe3\x82\x84\xe3\x81\xab", keys[1]);
  // "名古屋に"
  EXPECT_EQ("\xe5\x90\x8d\xe5\x8f\xa4\xe5\xb1\x8b\xe3\x81\xab", values[1]);
  // "なごや"
  EXPECT_EQ("\xe3\x81\xaa\xe3\x81\x94\xe3\x82\x84", content_keys[1]);
  // "名古屋"
  EXPECT_EQ("\xe5\x90\x8d\xe5\x8f\xa4\xe5\xb1\x8b", content_values[1]);

  // Inner segment 2: In the original segment, "行きたい" has the form
  // "行き" (content word) + "たい" (functional).  However, since "行き" is
  // Yougen, our rule for inner segment boundary doesn't handle it as a content
  // value.  Thus, "行きたい" becomes the content value.
  // "いきたい"
  EXPECT_EQ("\xe3\x81\x84\xe3\x81\x8d\xe3\x81\x9f\xe3\x81\x84", keys[2]);
  // "行きたい"
  EXPECT_EQ("\xe8\xa1\x8c\xe3\x81\x8d\xe3\x81\x9f\xe3\x81\x84", values[2]);
  // "いきたい"
  EXPECT_EQ("\xe3\x81\x84\xe3\x81\x8d\xe3\x81\x9f\xe3\x81\x84",
            content_keys[2]);
  // "行きたい"
  EXPECT_EQ("\xe8\xa1\x8c\xe3\x81\x8d\xe3\x81\x9f\xe3\x81\x84",
            content_values[2]);
}

}  // namespace mozc
