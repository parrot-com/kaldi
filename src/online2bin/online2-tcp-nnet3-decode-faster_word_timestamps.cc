// online2bin/online2-tcp-nnet3-decode-faster.cc

// Copyright 2014  Johns Hopkins University (author: Daniel Povey)
//           2016  Api.ai (Author: Ilya Platonov)
//           2018  Polish-Japanese Academy of Information Technology (Author: Danijel Korzinek)

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "feat/wave-reader.h"
#include "online2/online-nnet3-decoding.h"
#include "online2/online-nnet2-feature-pipeline.h"
#include "online2/onlinebin-util.h"
#include "online2/online-timing.h"
#include "online2/online-endpoint.h"
#include "fstext/fstext-lib.h"
#include "lat/lattice-functions.h"
#include "util/kaldi-thread.h"
#include "lat/word-align-lattice.h"
#include "nnet3/nnet-utils.h"
#include <sys/time.h>
#include <iostream>
#include <chrono>
#include <online2/json.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <poll.h>
#include <signal.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <random>
#include <sstream>

namespace uuid {
    static std::random_device              rd;
    static std::mt19937                    gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);

    std::string generate_uuid_v4() {
        std::stringstream ss;
        int i;
        ss << std::hex;
        for (i = 0; i < 8; i++) {
            ss << dis(gen);
        }
        ss << "-";
        for (i = 0; i < 4; i++) {
            ss << dis(gen);
        }
        ss << "-4";
        for (i = 0; i < 3; i++) {
            ss << dis(gen);
        }
        ss << "-";
        ss << dis2(gen);
        for (i = 0; i < 3; i++) {
            ss << dis(gen);
        }
        ss << "-";
        for (i = 0; i < 12; i++) {
            ss << dis(gen);
        };
        return ss.str();
    }
}


namespace kaldi {

class TcpServer {
 public:
  explicit TcpServer(int read_timeout);
  ~TcpServer();

  bool Listen(int32 port);  // start listening on a given port
  int32 Accept();  // accept a client and return its descriptor

  bool ReadChunk(size_t len); // get more data and return false if end-of-stream

  Vector<BaseFloat> GetChunk(); // get the data read by above method

  bool Write(const std::string &msg); // write to accepted client
  bool WriteLn(const nlohmann::json &msg, const std::string &eol = "\n"); // write line to accepted client

  void Disconnect();

 private:
  struct ::sockaddr_in h_addr_;
  int32 server_desc_, client_desc_;
  int16 *samp_buf_;
  size_t buf_len_, has_read_;
  pollfd client_set_[1];
  int read_timeout_;
};

std::string LatticeToString(const Lattice &lat, const fst::SymbolTable &word_syms) {
  LatticeWeight weight;
  std::vector<int32> alignment;
  std::vector<int32> words;
  GetLinearSymbolSequence(lat, &alignment, &words, &weight);

  std::ostringstream msg;
  for (size_t i = 0; i < words.size(); i++) {
    std::string s = word_syms.Find(words[i]);
    if (s.empty()) {
      KALDI_WARN << "Word-id " << words[i] << " not in symbol table.";
      msg << "<#" << std::to_string(i) << "> ";
    } else
      msg << s << " ";
  }
  return msg.str();
}

std::string GetTimeString(int32 t_beg, int32 t_end, BaseFloat time_unit) {
  char buffer[100];
  double t_beg2 = t_beg * time_unit;
  double t_end2 = t_end * time_unit;
  snprintf(buffer, 100, "%.2f %.2f", t_beg2, t_end2);
  return std::string(buffer);
}

int32 GetLatticeTimeSpan(const Lattice& lat) {
  std::vector<int32> times;
  LatticeStateTimes(lat, &times);
  return times.back();
}

std::string LatticeToString(const CompactLattice &clat, const fst::SymbolTable &word_syms) {
  if (clat.NumStates() == 0) {
    KALDI_WARN << "Empty lattice.";
    return "";
  }
  CompactLattice best_path_clat;
  CompactLatticeShortestPath(clat, &best_path_clat);

  Lattice best_path_lat;
  ConvertLattice(best_path_clat, &best_path_lat);
  return LatticeToString(best_path_lat, word_syms);
}
}

int main(int argc, char *argv[]) {
  try {
    using namespace kaldi;
    using namespace uuid;
    using namespace fst;
    using namespace std::chrono;
    using json = nlohmann::json;

    typedef kaldi::int32 int32;
    typedef kaldi::int64 int64;

    const char *usage =
        "Reads in audio from a network socket and performs online\n"
        "decoding with neural nets (nnet3 setup), with iVector-based\n"
        "speaker adaptation and endpointing.\n"
        "Note: some configuration values and inputs are set via config\n"
        "files whose filenames are passed as options\n"
        "\n"
        "Usage: online2-tcp-nnet3-decode-faster [options] <nnet3-in> "
        "<fst-in> <word-symbol-table>\n";

    ParseOptions po(usage);


    // feature_opts includes configuration for the iVector adaptation,
    // as well as the basic features.
    OnlineNnet2FeaturePipelineConfig feature_opts;
    nnet3::NnetSimpleLoopedComputationOptions decodable_opts;
    LatticeFasterDecoderConfig decoder_opts;
    OnlineEndpointConfig endpoint_opts;

    BaseFloat chunk_length_secs = 0.18;
    BaseFloat output_period = 1;
    BaseFloat samp_freq = 16000.0;
    int port_num = 5050;
    int read_timeout = 3;
    bool produce_time = false;
    std::string word_boundary = "";

    po.Register("samp-freq", &samp_freq,
                "Sampling frequency of the input signal (coded as 16-bit slinear).");
    po.Register("chunk-length", &chunk_length_secs,
                "Length of chunk size in seconds, that we process.");
    po.Register("output-period", &output_period,
                "How often in seconds, do we check for changes in output.");
    po.Register("num-threads-startup", &g_num_threads,
                "Number of threads used when initializing iVector extractor.");
    po.Register("read-timeout", &read_timeout,
                "Number of seconds of timeout for TCP audio data to appear on the stream. Use -1 for blocking.");
    po.Register("port-num", &port_num,
                "Port number the server will listen on.");
    po.Register("produce-time", &produce_time,
                "Prepend begin/end times between endpoints (e.g. '5.46 6.81 <text_output>', in seconds)");
    po.Register("word-boundary", &word_boundary,
                "path to a word boundary file");

    feature_opts.Register(&po);
    decodable_opts.Register(&po);
    decoder_opts.Register(&po);
    endpoint_opts.Register(&po);

    po.Read(argc, argv);

    if (po.NumArgs() != 3) {
      po.PrintUsage();
      return 1;
    }

    std::string nnet3_rxfilename = po.GetArg(1),
        fst_rxfilename = po.GetArg(2),
        word_syms_filename = po.GetArg(3);

    OnlineNnet2FeaturePipelineInfo feature_info(feature_opts);
    const string word_boundary_filename = word_boundary;
    WordBoundaryInfoNewOpts opts; // use default opts
    WordBoundaryInfo word_boundary_info(opts, word_boundary_filename);

    BaseFloat frame_shift = feature_info.FrameShiftInSeconds();
    int32 frame_subsampling = decodable_opts.frame_subsampling_factor;

    KALDI_VLOG(1) << "Loading AM...";

    TransitionModel trans_model;
    nnet3::AmNnetSimple am_nnet;
    {
      bool binary;
      Input ki(nnet3_rxfilename, &binary);
      trans_model.Read(ki.Stream(), binary);
      am_nnet.Read(ki.Stream(), binary);
      SetBatchnormTestMode(true, &(am_nnet.GetNnet()));
      SetDropoutTestMode(true, &(am_nnet.GetNnet()));
      nnet3::CollapseModel(nnet3::CollapseModelConfig(), &(am_nnet.GetNnet()));
    }

    // this object contains precomputed stuff that is used by all decodable
    // objects.  It takes a pointer to am_nnet because if it has iVectors it has
    // to modify the nnet to accept iVectors at intervals.
    nnet3::DecodableNnetSimpleLoopedInfo decodable_info(decodable_opts,
                                                        &am_nnet);

    KALDI_VLOG(1) << "Loading FST...";

    fst::Fst<fst::StdArc> *decode_fst = ReadFstKaldiGeneric(fst_rxfilename);

    fst::SymbolTable *word_syms = NULL;
    if (!word_syms_filename.empty())
      if (!(word_syms = fst::SymbolTable::ReadText(word_syms_filename)))
        KALDI_ERR << "Could not read symbol table from file "
                  << word_syms_filename;

    signal(SIGPIPE, SIG_IGN); // ignore SIGPIPE to avoid crashing when socket forcefully disconnected

    TcpServer server(read_timeout);

    server.Listen(port_num);

    while (true) {

      server.Accept();

      int32 samp_count = 0;// this is used for output refresh rate
      size_t chunk_len = static_cast<size_t>(chunk_length_secs * samp_freq);
      int32 check_period = static_cast<int32>(samp_freq * output_period);
      int32 check_count = check_period;

      int32 frame_offset = 0;

      bool eos = false;
      int word_count = 0;
      int block = 0;
      double last_timestamp = 0.0;
      std::string current_hypothesis = "";
      std::string global_message = "";
      std::string global_block_start = "";
      std::string global_block_end = "";
      std::string block_uuid = generate_uuid_v4();
      auto transcription_start = high_resolution_clock::now();

      OnlineNnet2FeaturePipeline feature_pipeline(feature_info);
      SingleUtteranceNnet3Decoder decoder(decoder_opts, trans_model,
                                          decodable_info,
                                          *decode_fst, &feature_pipeline);

      while (!eos) {

        decoder.InitDecoding(frame_offset);
        OnlineSilenceWeighting silence_weighting(
            trans_model,
            feature_info.silence_weighting_config,
            decodable_opts.frame_subsampling_factor);
        std::vector<std::pair<int32, BaseFloat>> delta_weights;

        while (true) {
          eos = !server.ReadChunk(chunk_len);

          if (eos) {
            feature_pipeline.InputFinished();

            if (silence_weighting.Active() &&
                feature_pipeline.IvectorFeature() != NULL) {
              silence_weighting.ComputeCurrentTraceback(decoder.Decoder());
              silence_weighting.GetDeltaWeights(feature_pipeline.NumFramesReady(),
                                                frame_offset * decodable_opts.frame_subsampling_factor,
                                                &delta_weights);
              feature_pipeline.UpdateFrameWeights(delta_weights);
            }

            decoder.AdvanceDecoding();
            decoder.FinalizeDecoding();
            frame_offset += decoder.NumFramesDecoded();
            if (decoder.NumFramesDecoded() > 0) {

              CompactLattice lat;
              decoder.GetLattice(true, &lat);
              TopSort(&lat); // for LatticeStateTimes(),
              std::string msg = LatticeToString(lat, *word_syms);
              istringstream iss(msg);
              vector<string> message_vector{istream_iterator<string>{iss}, istream_iterator<string>{}};
              current_hypothesis = current_hypothesis + ",";

              for (int i = word_count; i < message_vector.size(); i++) {
                // Hack to deal with end of audio file timestamps
                // we take the last timestamp from previous decoding and total duration of the audio, then uniformly assign 
                // durations to the remaining words

                double offset;
                std::string word = message_vector[i];
                if (word.find("<") == 0){
                    continue;
                }
                std::string str_start, str_end;

                if (message_vector.size() - word_count > 0) {
                  offset = (frame_offset * frame_subsampling * frame_shift - last_timestamp) / (message_vector.size() - word_count);
                }
                else {
                  offset = 0.0;
                }
                str_start = std::to_string(last_timestamp);
                last_timestamp = last_timestamp + offset;
                str_end = std::to_string(last_timestamp);
                current_hypothesis = current_hypothesis + "{\"word\":" + "\"" + word + "\"," + "\"start\":" + str_start + "," + "\"end\":" + str_end + "},";
                word_count += 1;
                global_block_end = str_end;
              }

              if (word_count > 0) {
              current_hypothesis.erase(std::prev(current_hypothesis.end()));
              struct timeval tp;
              gettimeofday(&tp, NULL);
              long int timestamp = tp.tv_sec * 1000 + tp.tv_usec / 1000;
              std::string current_block = std::to_string(block);
              std::string block_identifier = std::to_string(timestamp);
              current_hypothesis = "\"block_uuid\":\"" + block_uuid + "\"" + ", " +  "\"block\":" + current_block
                  + ", " + "\"timestamp\": " + block_identifier + ", " + ", \"first_word_in_block_start\": " + global_block_start
                  + ", \"last_word_in_block_end\": " + global_block_end + ", " + "\"words\":[" + current_hypothesis + "]}";
              global_message = current_hypothesis;

              auto transcript_time = high_resolution_clock::now();
              auto duration = duration_cast<milliseconds>( transcript_time - transcription_start ).count();
              std::string current_duration = std::to_string(duration);

              current_hypothesis = "{\"block_end\": true, \"time_from_beginning\": " + current_duration + ", " + current_hypothesis;

              bool jv = json::accept(current_hypothesis);
              if (jv) {
                server.Write(current_hypothesis);
                KALDI_VLOG(1) << "EndOfAudio, sending message: " << current_hypothesis;
                }
              else {
                KALDI_VLOG(1) << "Warning: Invalid json format encountered " << current_hypothesis;
                }
            }
            else {
                current_hypothesis = "";
            }
            }
            server.Disconnect();
            break;
          }

          Vector<BaseFloat> wave_part = server.GetChunk();
          feature_pipeline.AcceptWaveform(samp_freq, wave_part);
          samp_count += chunk_len;

          if (silence_weighting.Active() &&
              feature_pipeline.IvectorFeature() != NULL) {
            silence_weighting.ComputeCurrentTraceback(decoder.Decoder());
            silence_weighting.GetDeltaWeights(feature_pipeline.NumFramesReady(),
                                              frame_offset * decodable_opts.frame_subsampling_factor,
                                              &delta_weights);
            feature_pipeline.UpdateFrameWeights(delta_weights);
          }

          decoder.AdvanceDecoding();

          if (samp_count > check_count) {
            if (decoder.NumFramesDecoded() > 0) {
              double start_shift = frame_offset * frame_subsampling * frame_shift;
              Lattice lat;
              decoder.GetBestPath(false, &lat);

              CompactLattice clat; // make empty compact lattice
              ConvertLattice(lat, &clat); // convert lattice to compact lattice and save to clat

              CompactLattice aligned_clat;
              std::vector<int32> words, times, lengths; // define vectors
              std::vector<std::vector<int32> > prons;
              std::vector<std::vector<int32> > phone_lengths;

              WordAlignLattice(clat, trans_model, word_boundary_info, 0, &aligned_clat); // Align lattice by words
              CompactLatticeToWordProns(trans_model, aligned_clat, &words, &times, &lengths, &prons, &phone_lengths);

              TopSort(&lat); // for LatticeStateTimes(),
              std::string msg = LatticeToString(lat, *word_syms);
              istringstream iss(msg);
              vector<string> message_vector{istream_iterator<string>{iss}, istream_iterator<string>{}};
              std::string message = "";
              std::string first_word_in_block_start = "";
              std::string last_word_in_block_end = "";
              bool start_of_block_assigned = false;
              word_count = 0;

              for (int i = 0; i < words.size(); i++) {
                if(words[i] == 0) {
                continue;
                }
                std::string word = word_syms->Find(words[i]).c_str();
                if (word.find("<") == 0){
                    continue;
                }
                std::string str_start = std::to_string(times[i] * frame_shift * frame_subsampling + start_shift);
                std::string str_end = std::to_string((times[i] + lengths[i]) * frame_shift * frame_subsampling + start_shift);
                if (!start_of_block_assigned) {
                    first_word_in_block_start = str_start;
                    start_of_block_assigned = true;
                }
                message = message + "{\"word\":" + "\"" + word + "\"" + "," + "\"start\":" + str_start + "," + "\"end\":" + str_end + "},";
                last_timestamp = (times[i] + lengths[i]) * frame_shift * frame_subsampling;
                last_word_in_block_end = str_end;
                word_count += 1; // we want to remember the number of words in each lattice hypothesis
              }

              // remove trailing comma
              if (word_count > 0) {

                message.erase(std::prev(message.end()));
                current_hypothesis = message;
                global_block_start = first_word_in_block_start;
                global_block_end = last_word_in_block_end;
                struct timeval tp;
                gettimeofday(&tp, NULL);
                long int timestamp = tp.tv_sec * 1000 + tp.tv_usec / 1000;
                std::string current_block = std::to_string(block);
                std::string block_identifier = std::to_string(timestamp);
                message = "\"block_uuid\":\"" + block_uuid + "\"" + ", " + "\"block\":" + current_block + ", "  + "\"timestamp\": "
                    + block_identifier + ", \"first_word_in_block_start\": " + first_word_in_block_start
                    + ", \"last_word_in_block_end\": " + last_word_in_block_end
                    + ", " + "\"words\":[" + message + "]}";
                global_message = message;

                auto transcript_time = high_resolution_clock::now();
                auto duration = duration_cast<milliseconds>( transcript_time - transcription_start ).count();
                std::string current_duration = std::to_string(duration);

                message = "{\"block_end\": false, \"time_from_beginning\": " + current_duration + ", " + message;

                bool jv = json::accept(message);
                if (jv) {
                    server.Write(message);
                    KALDI_VLOG(1) << "Temporary transcript: " << message;
                }
                else {
                   KALDI_VLOG(1) << "Warning: Invalid json format encountered " << message;
                }
              }
            }
            check_count += check_period;
          }

          if (decoder.EndpointDetected(endpoint_opts)) {
            block += 1;
            block_uuid = generate_uuid_v4();
            decoder.FinalizeDecoding();
            frame_offset += decoder.NumFramesDecoded();
            CompactLattice lat;
            decoder.GetLattice(true, &lat);
            std::string msg = LatticeToString(lat, *word_syms);

            auto transcript_time = high_resolution_clock::now();
            auto duration = duration_cast<milliseconds>( transcript_time - transcription_start ).count();
            std::string current_duration = std::to_string(duration);

            if (word_count > 0) {
                global_message = "{\"block_end\": true, \"time_from_beginning\": " + current_duration + ", " + global_message;

                bool jv = json::accept(global_message);
                if (jv) {
                    server.Write(global_message);
                    KALDI_VLOG(1) << "Endpoint, sending message: " << global_message;
                    break;
                }
                else {
                  KALDI_VLOG(1) << "Warning: Invalid json format encountered " << global_message;
                  break;
                }
              }
            else {
                global_message = "";
                break;
             }
           }
         }
      }
    }
  } catch (const std::exception &e) {
    std::cerr << e.what();
    return -1;
  }
} // main()


namespace kaldi {
TcpServer::TcpServer(int read_timeout) {
  server_desc_ = -1;
  client_desc_ = -1;
  samp_buf_ = NULL;
  buf_len_ = 0;
  read_timeout_ = 1000 * read_timeout;
}

bool TcpServer::Listen(int32 port) {
  h_addr_.sin_addr.s_addr = INADDR_ANY;
  h_addr_.sin_port = htons(port);
  h_addr_.sin_family = AF_INET;

  server_desc_ = socket(AF_INET, SOCK_STREAM, 0);

  if (server_desc_ == -1) {
    KALDI_ERR << "Cannot create TCP socket!";
    return false;
  }

  int32 flag = 1;
  int32 len = sizeof(int32);
  if (setsockopt(server_desc_, SOL_SOCKET, SO_REUSEADDR, &flag, len) == -1) {
    KALDI_ERR << "Cannot set socket options!";
    return false;
  }

  if (bind(server_desc_, (struct sockaddr *) &h_addr_, sizeof(h_addr_)) == -1) {
    KALDI_ERR << "Cannot bind to port: " << port << " (is it taken?)";
    return false;
  }

  if (listen(server_desc_, 1) == -1) {
    KALDI_ERR << "Cannot listen on port!";
    return false;
  }

  KALDI_LOG << "TcpServer: Listening on port: " << port;

  return true;

}

TcpServer::~TcpServer() {
  Disconnect();
  if (server_desc_ != -1)
    close(server_desc_);
  delete[] samp_buf_;
}

int32 TcpServer::Accept() {
  KALDI_LOG << "Waiting for client...";

  socklen_t len;

  len = sizeof(struct sockaddr);
  client_desc_ = accept(server_desc_, (struct sockaddr *) &h_addr_, &len);

  struct sockaddr_storage addr;
  char ipstr[20];

  len = sizeof addr;
  getpeername(client_desc_, (struct sockaddr *) &addr, &len);

  struct sockaddr_in *s = (struct sockaddr_in *) &addr;
  inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);

  client_set_[0].fd = client_desc_;
  client_set_[0].events = POLLIN;

  KALDI_LOG << "Accepted connection from: " << ipstr;

  return client_desc_;
}

bool TcpServer::ReadChunk(size_t len) {
  if (buf_len_ != len) {
    buf_len_ = len;
    delete[] samp_buf_;
    samp_buf_ = new int16[len];
  }

  ssize_t ret;
  int poll_ret;
  char *samp_buf_p = reinterpret_cast<char *>(samp_buf_);
  size_t to_read = len * sizeof(int16);
  has_read_ = 0;
  while (to_read > 0) {
    poll_ret = poll(client_set_, 1, read_timeout_);
    if (poll_ret == 0) {
      KALDI_WARN << "Socket timeout! Disconnecting..." << "(has_read_ = " << has_read_ << ")";
      break;
    }
    if (poll_ret < 0) {
      KALDI_WARN << "Socket error! Disconnecting...";
      break;
    }
    ret = read(client_desc_, static_cast<void *>(samp_buf_p + has_read_), to_read);
    if (ret <= 0) {
      KALDI_WARN << "Stream over...";
      break;
    }
    to_read -= ret;
    has_read_ += ret;
  }
  has_read_ /= sizeof(int16);

  return has_read_ > 0;
}

Vector<BaseFloat> TcpServer::GetChunk() {
  Vector<BaseFloat> buf;

  buf.Resize(static_cast<MatrixIndexT>(has_read_));

  for (int i = 0; i < has_read_; i++)
    buf(i) = static_cast<BaseFloat>(samp_buf_[i]);

  return buf;
}

bool TcpServer::Write(const std::string &msg) {

  const char *p = msg.c_str();
  size_t to_write = msg.size();
  size_t wrote = 0;
  while (to_write > 0) {
    ssize_t ret = write(client_desc_, static_cast<const void *>(p + wrote), to_write);
    if (ret <= 0)
      return false;

    to_write -= ret;
    wrote += ret;
  }

  return true;
}

bool TcpServer::WriteLn(const nlohmann::json &msg, const std::string &eol) {
  if (Write(msg))
    return Write(eol);
  else return false;
}

void TcpServer::Disconnect() {
  if (client_desc_ != -1) {
    close(client_desc_);
    client_desc_ = -1;
  }
}
}  // namespace kaldi
