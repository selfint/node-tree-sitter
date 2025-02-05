#include "./parser.h"
#include <string>
#include <vector>
#include <climits>
#include <v8.h>
#include <nan.h>
#include "./conversions.h"
#include "./language.h"
#include "./logger.h"
#include "./tree.h"
#include "./util.h"
#include "text-buffer-snapshot-wrapper.h"
#include <cmath>

namespace node_tree_sitter {

using namespace v8;
using std::vector;
using std::pair;

Nan::Persistent<Function> Parser::constructor;

class CallbackInput {
 public:
  CallbackInput(v8::Local<v8::Function> callback, v8::Local<v8::Value> js_buffer_size)
    : callback(callback),
      byte_offset(0),
      partial_string_offset(0) {
    uint32_t buffer_size = Nan::To<uint32_t>(js_buffer_size).FromMaybe(0);
    if (buffer_size == 0) buffer_size = 32 * 1024;
    buffer.resize(buffer_size);
  }

  TSInput Input() {
    TSInput result;
    result.payload = (void *)this;
    result.encoding = TSInputEncodingUTF16;
    result.read = Read;
    return result;
  }

 private:
  static const char * Read(void *payload, uint32_t byte, TSPoint position, uint32_t *bytes_read) {
    CallbackInput *reader = (CallbackInput *)payload;

    if (byte != reader->byte_offset) {
      reader->byte_offset = byte;
      reader->partial_string_offset = 0;
      reader->partial_string.Reset();
    }

    *bytes_read = 0;
    Local<String> result;
    uint32_t start = 0;
    if (reader->partial_string_offset) {
      result = Nan::New(reader->partial_string);
      start = reader->partial_string_offset;
    } else {
      Local<Function> callback = Nan::New(reader->callback);
      uint32_t utf16_unit = byte / 2;
      Local<Value> argv[2] = { Nan::New<Number>(utf16_unit), PointToJS(position) };
      TryCatch try_catch(Isolate::GetCurrent());
      auto maybe_result_value = Nan::Call(callback, GetGlobal(callback), 2, argv);
      if (try_catch.HasCaught()) return nullptr;

      Local<Value> result_value;
      if (!maybe_result_value.ToLocal(&result_value)) return nullptr;
      if (!result_value->IsString()) return nullptr;
      if (!Nan::To<String>(result_value).ToLocal(&result)) return nullptr;
    }

    int utf16_units_read = result->Write(

      // Nan doesn't wrap this functionality
      #if NODE_MAJOR_VERSION >= 12
        Isolate::GetCurrent(),
      #endif

      reader->buffer.data(),
      start,
      reader->buffer.size(),
      String::NO_NULL_TERMINATION
    );
    int end = start + utf16_units_read;
    *bytes_read = 2 * utf16_units_read;

    reader->byte_offset += *bytes_read;

    if (end < result->Length()) {
      reader->partial_string_offset = end;
      reader->partial_string.Reset(result);
    } else {
      reader->partial_string_offset = 0;
      reader->partial_string.Reset();
    }

    return (const char *)reader->buffer.data();
  }

  Nan::Persistent<v8::Function> callback;
  std::vector<uint16_t> buffer;
  size_t byte_offset;
  Nan::Persistent<v8::String> partial_string;
  size_t partial_string_offset;
};

class TextBufferInput {
public:
  TextBufferInput(const vector<pair<const char16_t *, uint32_t>> *slices)
    : slices_(slices),
      byte_offset(0),
      slice_index_(0),
      slice_offset_(0) {}

  TSInput input() {
    return TSInput{this, Read, TSInputEncodingUTF16};
  }

private:
  void seek(uint32_t byte_offset) {
    this->byte_offset = byte_offset;

    uint32_t total_length = 0;
    uint32_t goal_index = byte_offset / 2;
    for (unsigned i = 0, n = this->slices_->size(); i < n; i++) {
      uint32_t next_total_length = total_length + this->slices_->at(i).second;
      if (next_total_length > goal_index) {
        this->slice_index_ = i;
        this->slice_offset_ = goal_index - total_length;
        return;
      }
      total_length = next_total_length;
    }

    this->slice_index_ = this->slices_->size();
    this->slice_offset_ = 0;
  }

  static const char *Read(void *payload, uint32_t byte, TSPoint position, uint32_t *length) {
    auto self = static_cast<TextBufferInput *>(payload);

    if (byte != self->byte_offset) self->seek(byte);

    if (self->slice_index_ == self->slices_->size()) {
      *length = 0;
      return "";
    }

    auto &slice = self->slices_->at(self->slice_index_);
    const char16_t *result = slice.first + self->slice_offset_;
    *length = 2 * (slice.second - self->slice_offset_);
    self->byte_offset += *length;
    self->slice_index_++;
    self->slice_offset_ = 0;
    return reinterpret_cast<const char *>(result);
  }

  const vector<pair<const char16_t *, uint32_t>> *slices_;
  uint32_t byte_offset;
  uint32_t slice_index_;
  uint32_t slice_offset_;
};

void Parser::Init(Local<Object> exports) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  tpl->InstanceTemplate()->SetInternalFieldCount(1);
  Local<String> class_name = Nan::New("Parser").ToLocalChecked();
  tpl->SetClassName(class_name);

  FunctionPair methods[] = {
    {"getLogger", GetLogger},
    {"setLogger", SetLogger},
    {"setLanguage", SetLanguage},
    {"printDotGraphs", PrintDotGraphs},
    {"parse", Parse},
    {"parseTextBuffer", ParseTextBuffer},
    {"parseTextBufferSync", ParseTextBufferSync},
  };

  for (size_t i = 0; i < length_of_array(methods); i++) {
    Nan::SetPrototypeMethod(tpl, methods[i].name, methods[i].callback);
  }

  constructor.Reset(Nan::Persistent<Function>(Nan::GetFunction(tpl).ToLocalChecked()));
  Nan::Set(exports, class_name, Nan::New(constructor));
  Nan::Set(exports, Nan::New("LANGUAGE_VERSION").ToLocalChecked(), Nan::New<Number>(TREE_SITTER_LANGUAGE_VERSION));
}

Parser::Parser() : parser_(ts_parser_new()), is_parsing_async_(false) {}

Parser::~Parser() { ts_parser_delete(parser_); }

static bool handle_included_ranges(TSParser *parser, Local<Value> arg) {
  uint32_t last_included_range_end = 0;
  if (arg->IsArray()) {
    auto js_included_ranges = Local<Array>::Cast(arg);
    vector<TSRange> included_ranges;
    for (unsigned i = 0; i < js_included_ranges->Length(); i++) {
      Local<Value> range_value;
      if (!Nan::Get(js_included_ranges, i).ToLocal(&range_value)) return false;
      auto maybe_range = RangeFromJS(range_value);
      if (!maybe_range.IsJust()) return false;
      auto range = maybe_range.FromJust();
      if (range.start_byte < last_included_range_end) {
        Nan::ThrowRangeError("Overlapping ranges");
        return false;
      }
      last_included_range_end = range.end_byte;
      included_ranges.push_back(range);
    }
    ts_parser_set_included_ranges(parser, included_ranges.data(), included_ranges.size());
  } else {
    ts_parser_set_included_ranges(parser, nullptr, 0);
  }

  return true;
}

void Parser::New(const Nan::FunctionCallbackInfo<Value> &info) {
  if (info.IsConstructCall()) {
    Parser *parser = new Parser();
    parser->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  } else {
    Local<Object> self;
    MaybeLocal<Object> maybe_self = Nan::New(constructor)->NewInstance(Nan::GetCurrentContext());
    if (maybe_self.ToLocal(&self)) {
      info.GetReturnValue().Set(self);
    } else {
      info.GetReturnValue().Set(Nan::Null());
    }
  }
}

void Parser::SetLanguage(const Nan::FunctionCallbackInfo<Value> &info) {
  Parser *parser = ObjectWrap::Unwrap<Parser>(info.This());
  if (parser->is_parsing_async_) {
    Nan::ThrowError("Parser is in use");
    return;
  }

  const TSLanguage *language = language_methods::UnwrapLanguage(info[0]);
  if (language) {
    ts_parser_set_language(parser->parser_, language);
    info.GetReturnValue().Set(info.This());
  }
}

void Parser::Parse(const Nan::FunctionCallbackInfo<Value> &info) {
  Parser *parser = ObjectWrap::Unwrap<Parser>(info.This());
  if (parser->is_parsing_async_) {
    Nan::ThrowError("Parser is in use");
    return;
  }

  if (!info[0]->IsFunction()) {
    Nan::ThrowTypeError("Input must be a function");
    return;
  }

  Local<Function> callback = Local<Function>::Cast(info[0]);

  Local<Object> js_old_tree;
  const TSTree *old_tree = nullptr;
  if (info.Length() > 1 && !info[1]->IsNull() && !info[1]->IsUndefined() && Nan::To<Object>(info[1]).ToLocal(&js_old_tree)) {
    const Tree *tree = Tree::UnwrapTree(js_old_tree);
    if (!tree) {
      Nan::ThrowTypeError("Second argument must be a tree");
      return;
    }
    old_tree = tree->tree_;
  }

  Local<Value> buffer_size = Nan::Null();
  if (info.Length() > 2) buffer_size = info[2];

  if (!handle_included_ranges(parser->parser_, info[3])) return;

  CallbackInput callback_input(callback, buffer_size);
  TSTree *tree = ts_parser_parse(parser->parser_, old_tree, callback_input.Input());
  Local<Value> result = Tree::NewInstance(tree);
  info.GetReturnValue().Set(result);
}

class ParseWorker : public Nan::AsyncWorker {
  Parser *parser_;
  TSTree *new_tree_;
  TextBufferInput *input_;

public:
  ParseWorker(Nan::Callback *callback, Parser *parser, TextBufferInput *input) :
    AsyncWorker(callback, "tree-sitter.parseTextBuffer"),
    parser_(parser),
    new_tree_(nullptr),
    input_(input) {}

  void Execute() {
    TSLogger logger = ts_parser_logger(parser_->parser_);
    ts_parser_set_logger(parser_->parser_, TSLogger{0, 0});
    new_tree_ = ts_parser_parse(parser_->parser_, nullptr, input_->input());
    ts_parser_set_logger(parser_->parser_, logger);
  }

  void HandleOKCallback() {
    parser_->is_parsing_async_ = false;
    delete input_;
    Local<Value> argv[] = {Tree::NewInstance(new_tree_)};
    callback->Call(1, argv, async_resource);
  }
};

void Parser::ParseTextBuffer(const Nan::FunctionCallbackInfo<Value> &info) {
  Parser *parser = ObjectWrap::Unwrap<Parser>(info.This());
  if (parser->is_parsing_async_) {
    Nan::ThrowError("Parser is in use");
    return;
  }

  Local<Object> js_old_tree;
  const TSTree *old_tree = nullptr;
  if (info.Length() > 2 && info[2]->IsObject() && Nan::To<Object>(info[2]).ToLocal(&js_old_tree)) {
    const Tree *tree = Tree::UnwrapTree(js_old_tree);
    if (!tree) {
      Nan::ThrowTypeError("Second argument must be a tree");
      return;
    }
    old_tree = tree->tree_;
  }

  if (!handle_included_ranges(parser->parser_, info[3])) return;

  auto snapshot = Nan::ObjectWrap::Unwrap<TextBufferSnapshotWrapper>(info[1].As<Object>());
  auto input = new TextBufferInput(snapshot->slices());

  // If a `syncTimeoutMicros` option is passed, parse synchronously
  // for the given amount of time before queuing an async task.
  double js_sync_timeout = Nan::To<double>(info[4]).FromMaybe(-1);
  if (js_sync_timeout > 0) {
    size_t sync_timeout;

    // If the timeout is `Infinity`, then parse synchronously with no timeout.
    if (js_sync_timeout && !std::isfinite(js_sync_timeout)) {
      sync_timeout = 0;
    } else {
      auto maybe_sync_timeout = Nan::To<uint32_t>(info[4]);
      if (maybe_sync_timeout.IsJust()) {
        sync_timeout = maybe_sync_timeout.FromJust();
      } else {
        Nan::ThrowTypeError("The `syncTimeoutMicros` option must be a positive integer.");
        return;
      }
    }

    // Logging is disabled for this method, because we can't call the
    // logging callback from an async worker.
    TSLogger logger = ts_parser_logger(parser->parser_);
    ts_parser_set_timeout_micros(parser->parser_, sync_timeout);
    ts_parser_set_logger(parser->parser_, TSLogger{0, 0});
    TSTree *result = ts_parser_parse(parser->parser_, old_tree, input->input());
    ts_parser_set_timeout_micros(parser->parser_, 0);
    ts_parser_set_logger(parser->parser_, logger);

    if (result) {
      delete input;
      Local<Value> argv[] = {Tree::NewInstance(result)};
      auto callback = info[0].As<Function>();
      Nan::Call(callback, GetGlobal(callback), 1, argv);
      return;
    }
  }

  auto callback = new Nan::Callback(info[0].As<Function>());
  parser->is_parsing_async_ = true;
  Nan::AsyncQueueWorker(new ParseWorker(
    callback,
    parser,
    input
  ));
}

void Parser::ParseTextBufferSync(const Nan::FunctionCallbackInfo<Value> &info) {
  Parser *parser = ObjectWrap::Unwrap<Parser>(info.This());
  if (parser->is_parsing_async_) {
    Nan::ThrowError("Parser is in use");
    return;
  }

  Local<Object> js_old_tree;
  const TSTree *old_tree = nullptr;
  if (info.Length() > 1 && info[1]->IsObject() && Nan::To<Object>(info[1]).ToLocal(&js_old_tree)) {
    const Tree *tree = Tree::UnwrapTree(js_old_tree);
    if (!tree) {
      Nan::ThrowTypeError("Second argument must be a tree");
      return;
    }
    old_tree = ts_tree_copy(tree->tree_);
  }

  if (!handle_included_ranges(parser->parser_, info[2])) return;

  auto snapshot = Nan::ObjectWrap::Unwrap<TextBufferSnapshotWrapper>(info[0].As<Object>());
  TextBufferInput input(snapshot->slices());
  TSTree *result = ts_parser_parse(parser->parser_, old_tree, input.input());
  info.GetReturnValue().Set(Tree::NewInstance(result));
}

void Parser::GetLogger(const Nan::FunctionCallbackInfo<Value> &info) {
  Parser *parser = ObjectWrap::Unwrap<Parser>(info.This());

  TSLogger current_logger = ts_parser_logger(parser->parser_);
  if (current_logger.payload && current_logger.log == Logger::Log) {
    Logger *logger = (Logger *)current_logger.payload;
    info.GetReturnValue().Set(Nan::New(logger->func));
  } else {
    info.GetReturnValue().Set(Nan::Null());
  }
}

void Parser::SetLogger(const Nan::FunctionCallbackInfo<Value> &info) {
  Parser *parser = ObjectWrap::Unwrap<Parser>(info.This());
  if (parser->is_parsing_async_) {
    Nan::ThrowError("Parser is in use");
    return;
  }

  TSLogger current_logger = ts_parser_logger(parser->parser_);

  if (info[0]->IsFunction()) {
    if (current_logger.payload) delete (Logger *)current_logger.payload;
    ts_parser_set_logger(parser->parser_, Logger::Make(Local<Function>::Cast(info[0])));
  } else if (!Nan::To<bool>(info[0]).FromMaybe(true)) {
    if (current_logger.payload) delete (Logger *)current_logger.payload;
    ts_parser_set_logger(parser->parser_, { 0, 0 });
  } else {
    Nan::ThrowTypeError("Logger callback must either be a function or a falsy value");
    return;
  }

  info.GetReturnValue().Set(info.This());
}

void Parser::PrintDotGraphs(const Nan::FunctionCallbackInfo<Value> &info) {
  Parser *parser = ObjectWrap::Unwrap<Parser>(info.This());
  if (parser->is_parsing_async_) {
    Nan::ThrowError("Parser is in use");
    return;
  }

  if (Nan::To<bool>(info[0]).FromMaybe(false)) {
    ts_parser_print_dot_graphs(parser->parser_, 2);
  } else {
    ts_parser_print_dot_graphs(parser->parser_, -1);
  }

  info.GetReturnValue().Set(info.This());
}

}  // namespace node_tree_sitter
