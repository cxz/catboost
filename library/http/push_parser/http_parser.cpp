#include "http_parser.h"

#include <util/generic/string.h>
#include <util/generic/yexception.h>
#include <util/stream/mem.h>
#include <util/stream/zlib.h>
#include <util/string/split.h>
#include <util/string/strip.h>

//#define DBGOUT(args) Cout << args << Endl;
#define DBGOUT(args)

namespace {
    const TString BestCodings[] = {
        "gzip",
        "deflate",
        "br",
        "x-gzip",
        "x-deflate",
        "y-lzo",
        "y-lzf",
        "y-lzq",
        "y-bzip2",
        "y-lzma",
    };
}

TString THttpParser::GetBestCompressionScheme() const {
    if (AcceptEncodings_.has("*")) {
        return BestCodings[0];
    }

    for (auto& coding : BestCodings) {
        if (AcceptEncodings_.has(coding)) {
            return coding;
        }
    }

    return TString();
}

bool THttpParser::FirstLineParser() {
    if (Y_UNLIKELY(!ReadLine())) {
        return false;
    }

    CurrentLine_.swap(FirstLine_);

    try {
        TStringBuf s(FirstLine_);
        if (MessageType_ == Response) {
            // Status-Line = HTTP-Version SP Status-Code SP Reason-Phrase CRLF
            TStringBuf httpVersion, statusCode;
            GetNext(s, ' ', httpVersion);
            ParseHttpVersion(httpVersion);
            GetNext(s, ' ', statusCode);
            RetCode_ = FromString<unsigned>(statusCode);
        } else {
            // Request-Line   = Method SP Request-URI SP HTTP-Version CRLF
            TStringBuf httpVersion = s.After(' ').After(' ');
            ParseHttpVersion(httpVersion);
        }
    } catch (...) {
        throw THttpParseException() << "Cannot parse first line: " << CurrentExceptionMessage() << " First 80 chars of line: " << FirstLine_.substr(0, Min<size_t>(80ull, +FirstLine_)).Quote();
    }

    return HeadersParser();
}

bool THttpParser::HeadersParser() {
    while (ReadLine()) {
        if (!CurrentLine_) {
            //end of headers
            DBGOUT("end of headers()");
            if (HasContentLength_) {
                if (ContentLength_ == 0) {
                    return OnEndParsing();
                }

                if (ContentLength_ < 1000000) {
                    Content_.reserve(ContentLength_ + 1);
                }
            }

            ParseHeaderLine();

            return !!ChunkInputState_ ? ChunkedContentParser() : ContentParser();
        }

        if (CurrentLine_[0] == ' ' || CurrentLine_[0] == '\t') {
            //continue previous header-line
            HeaderLine_ += CurrentLine_;
            CurrentLine_.remove(0);
        } else {
            ParseHeaderLine();
            HeaderLine_.swap(CurrentLine_);
        }
    }

    Parser_ = &THttpParser::HeadersParser;
    return false;
}

bool THttpParser::ContentParser() {
    DBGOUT("Content parsing()");
    if (HasContentLength_) {
        size_t rd = Min<size_t>(DataEnd_ - Data_, ContentLength_ - Content_.Size());
        Content_.append(Data_, rd);
        Data_ += rd;
        DBGOUT("Content parsing: " << Content_.Size() << " from " << ContentLength_);
        if (Content_.Size() == ContentLength_) {
            return OnEndParsing();
        }
    } else {
        if (MessageType_ == Request) {
            return OnEndParsing(); //RFC2616 4.4-5
        } else if (Y_UNLIKELY(RetCode() < 200 || RetCode() == 204 || RetCode() == 304)) {
            return OnEndParsing(); //RFC2616 4.4-1 (but not checked HEAD request type !)
        }

        Content_.append(Data_, DataEnd_);
        Data_ = DataEnd_;
    }
    Parser_ = &THttpParser::ContentParser;
    return false;
}

bool THttpParser::ChunkedContentParser() {
    DBGOUT("ReadChunkedContent");
    TChunkInputState& ci = *ChunkInputState_;

    if (Content_.capacity() < static_cast<size_t>(DataEnd_ - Data_)) {
        //try reduce memory reallocations
        Content_.reserve(DataEnd_ - Data_);
    }

    do {
        if (!ci.LeftBytes_) {
            if (Y_UNLIKELY(!ReadLine())) { //read first chunk size or CRLF from prev chunk or CRLF from last chunk
                break;
            }

            if (Y_UNLIKELY(ci.ReadLastChunk_)) {
                return OnEndParsing();
            }

            if (!CurrentLine_) {
                // skip crlf from previous chunk
                if (!ReadLine()) {
                    break;
                }
            }
            Y_ENSURE(+CurrentLine_, "NEH: LeftBytes hex number cannot be empty. ");
            size_t size = CurrentLine_.find_first_of(" \t;");
            if (size == TString::npos) {
                size = CurrentLine_.size();
            }
            ci.LeftBytes_ = IntFromString<ui32, 16, char>(CurrentLine_.c_str(), size);
            CurrentLine_.remove(0);
            if (!ci.LeftBytes_) { //detectect end of context marker - zero-size chunk, need read CRLF after empty chunk
                ci.ReadLastChunk_ = true;
                if (ReadLine()) {
                    return OnEndParsing();
                } else {
                    break;
                }
            }
        }

        size_t rd = Min<size_t>(DataEnd_ - Data_, ci.LeftBytes_);
        Content_.append(Data_, rd);
        Data_ += rd;
        ci.LeftBytes_ -= rd;
    } while (Data_ != DataEnd_);

    Parser_ = &THttpParser::ChunkedContentParser;
    return false;
}

bool THttpParser::OnEndParsing() {
    Parser_ = &THttpParser::OnEndParsing;
    ExtraDataSize_ = DataEnd_ - Data_;
    return true;
}

//continue read to CurrentLine_
bool THttpParser::ReadLine() {
    TStringBuf in(Data_, DataEnd_);
    size_t endl = in.find('\n');

    if (Y_UNLIKELY(endl == TStringBuf::npos)) {
        //input line not completed
        CurrentLine_.append(Data_, DataEnd_);
        return false;
    }

    CurrentLine_.append(~in, endl);
    if (Y_LIKELY(CurrentLine_.Size())) {
        //remove '\r' from tail
        size_t withoutCR = CurrentLine_.Size() - 1;
        if (CurrentLine_[withoutCR] == '\r') {
            CurrentLine_.remove(withoutCR);
        }
    }

    //Cout << "ReadLine:" << CurrentLine_ << Endl;
    Data_ += endl + 1;
    return true;
}

void THttpParser::ParseHttpVersion(TStringBuf httpVersion) {
    if (!httpVersion.StartsWith("HTTP/", 5)) {
        throw yexception() << "expect 'HTTP/'";
    }
    httpVersion.Skip(5);
    {
        TStringBuf major, minor;
        Split(httpVersion, '.', major, minor);
        HttpVersion_.Major = FromString<unsigned>(major);
        HttpVersion_.Minor = FromString<unsigned>(minor);
        if (Y_LIKELY(HttpVersion_.Major > 1 || HttpVersion_.Minor > 0)) {
            // since HTTP/1.1 Keep-Alive is default behaviour
            KeepAlive_ = true;
        }
    }
}

void THttpParser::ParseHeaderLine() {
    if (!!HeaderLine_) {
        if (CollectHeaders_) {
            THttpInputHeader hdr(HeaderLine_);

            Headers_.AddHeader(hdr);

            ApplyHeaderLine(to_lower(hdr.Name()), to_lower(hdr.Value()));
        } else {
            //some dirty optimization (avoid reallocation new strings)
            size_t pos = HeaderLine_.find(':');

            if (pos == TString::npos) {
                ythrow THttpParseException() << "can not parse http header(" << HeaderLine_.Quote() << ")";
            }

            HeaderLine_.to_lower();
            TStringBuf name(StripString(TStringBuf(HeaderLine_.begin(), HeaderLine_.begin() + pos)));
            TStringBuf val(StripString(TStringBuf(HeaderLine_.begin() + pos + 1, HeaderLine_.end())));
            ApplyHeaderLine(name, val);
        }
        HeaderLine_.remove(0);
    }
}

void THttpParser::OnEof() {
    if (Parser_ == &THttpParser::ContentParser && !HasContentLength_ && !ChunkInputState_) {
        return; //end of content determined by end of input
    }
    throw THttpException() << AsStringBuf("incompleted http response");
}

bool THttpParser::DecodeContent() {
    if (!ContentEncoding_) {
        DecodedContent_ = Content_;
        return false;
    }

    TMemoryInput in(~Content_, +Content_);
    if (ContentEncoding_ == "gzip") {
        DecodedContent_ = TZLibDecompress(&in, ZLib::GZip).ReadAll();
    } else if (ContentEncoding_ == "deflate") {
        DecodedContent_ = TZLibDecompress(&in, ZLib::ZLib).ReadAll();
    } else {
        throw THttpParseException() << "Unsupported content-encoding method: " << ContentEncoding_;
    }
    return true;
}

void THttpParser::ApplyHeaderLine(const TStringBuf& name, const TStringBuf& val) {
    if (name == AsStringBuf("connection")) {
        KeepAlive_ = val == AsStringBuf("keep-alive");
    } else if (name == AsStringBuf("content-length")) {
        Y_ENSURE(+val, "NEH: Content-Length cannot be empty string. ");
        ContentLength_ = FromString<ui64>(val);
        HasContentLength_ = true;
    } else if (name == AsStringBuf("transfer-encoding")) {
        if (val == AsStringBuf("chunked")) {
            ChunkInputState_ = new TChunkInputState();
        }
    } else if (name == AsStringBuf("accept-encoding")) {
        TStringBuf encodings(val);
        while (+encodings) {
            TStringBuf enc = encodings.NextTok(',').After(' ').Before(' ');
            if (!enc) {
                continue;
            }
            TString s(enc);
            s.to_lower();
            AcceptEncodings_.insert(s);
        }
    } else if (name == AsStringBuf("content-encoding")) {
        TString s(val);
        s.to_lower();
        ContentEncoding_ = s;
    }
}
