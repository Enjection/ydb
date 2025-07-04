#include "brotli.h"

#include <contrib/libs/brotli/c/include/brotli/decode.h>
#include <contrib/libs/brotli/c/include/brotli/encode.h>

#include <util/generic/yexception.h>
#include <util/memory/addstorage.h>

namespace {
    struct TAllocator {
        static void* Allocate(void* /* opaque */, size_t size) {
            return ::operator new(size);
        }

        static void Deallocate(void* /* opaque */, void* ptr) noexcept {
            ::operator delete(ptr);
        }
    };

}

class TBrotliCompress::TImpl {
public:
    TImpl(IOutputStream* slave,
          int quality,
          const TBrotliDictionary* dictionary,
          size_t offset = 0)
        : Slave_(slave)
        , EncoderState_(BrotliEncoderCreateInstance(&TAllocator::Allocate, &TAllocator::Deallocate, nullptr))
    {
        if (!EncoderState_) {
            ythrow yexception() << "Brotli encoder initialization failed";
        }

        if (dictionary) {
            BrotliEncoderAttachPreparedDictionary(EncoderState_, dictionary->GetPreparedDictionary());
        }

        SetEncoderParameter(BROTLI_PARAM_QUALITY, quality, "quality");
        SetEncoderParameter(BROTLI_PARAM_STREAM_OFFSET, offset, "stream offset");
    }

    ~TImpl() {
        FreeEncoder();
    }

    void Write(const void* buffer, size_t size) {
        DoWrite(buffer, size, BROTLI_OPERATION_PROCESS);
    }

    void Flush() {
        DoWrite(nullptr, 0, BROTLI_OPERATION_FLUSH);
    }

    void Finish() {
        Flush();
        DoWrite(nullptr, 0, BROTLI_OPERATION_FINISH);
        Y_ABORT_UNLESS(BrotliEncoderIsFinished(EncoderState_));
    }

private:
    IOutputStream* Slave_;
    BrotliEncoderState* EncoderState_;

    void SetEncoderParameter(BrotliEncoderParameter parameter,
                             ui32 value,
                             TStringBuf parameterName) {
        if (!BrotliEncoderSetParameter(EncoderState_, parameter, value)) {
            FreeEncoder();
            ythrow yexception() << "Failed to set brotli encoder " << parameterName << " to " << value;
        }
    }

    void FreeEncoder() {
        BrotliEncoderDestroyInstance(EncoderState_);
    }

    void DoWrite(const void* buffer, size_t size, BrotliEncoderOperation operation) {
        size_t availableOut = 0;
        ui8* outputBuffer = nullptr;

        const ui8* uBuffer = static_cast<const ui8*>(buffer);

        do {
            auto result = BrotliEncoderCompressStream(
                EncoderState_,
                operation,
                &size,
                &uBuffer,
                &availableOut,
                &outputBuffer,
                nullptr);

            if (result == BROTLI_FALSE) {
                ythrow yexception() << "Brotli encoder failed to process buffer";
            }

            size_t outputLength = 0;
            const ui8* output = BrotliEncoderTakeOutput(EncoderState_, &outputLength);
            if (outputLength > 0) {
                Slave_->Write(output, outputLength);
            }
        } while (size > 0 || BrotliEncoderHasMoreOutput(EncoderState_));
    }
};

TBrotliCompress::TBrotliCompress(IOutputStream* slave,
                                 int quality,
                                 const TBrotliDictionary* dictionary,
                                 size_t offset) {
    Impl_.Reset(new TImpl(slave,
                          quality,
                          dictionary,
                          offset));
}

TBrotliCompress::~TBrotliCompress() {
    try {
        Finish();
    } catch (...) {
    }
}

void TBrotliCompress::DoWrite(const void* buffer, size_t size) {
    Impl_->Write(buffer, size);
}

void TBrotliCompress::DoFlush() {
    if (Impl_) {
        Impl_->Flush();
    }
}

void TBrotliCompress::DoFinish() {
    THolder<TImpl> impl(Impl_.Release());

    if (impl) {
        impl->Finish();
    }
}

////////////////////////////////////////////////////////////////////////////////

class TBrotliDecompress::TImpl: public TAdditionalStorage<TImpl> {
public:
    TImpl(IInputStream* slave,
          const TBrotliDictionary* dictionary)
        : Slave_(slave)
        , Dictionary_(dictionary)
    {
        InitDecoder();
    }

    ~TImpl() {
        FreeDecoder();
    }

    size_t Read(void* buffer, size_t size) {
        Y_ASSERT(size > 0);

        ui8* outBuffer = static_cast<ui8*>(buffer);
        size_t availableOut = size;
        size_t decompressedSize = 0;

        BrotliDecoderResult result;
        do {
            if (InputAvailable_ == 0 && !InputExhausted_) {
                InputBuffer_ = TmpBuf();
                InputAvailable_ = Slave_->Read((void*)InputBuffer_, TmpBufLen());
                if (InputAvailable_ == 0) {
                    InputExhausted_ = true;
                }
            }

            if (SubstreamFinished_ && !InputExhausted_) {
                ResetState();
            }

            result = BrotliDecoderDecompressStream(
                DecoderState_,
                &InputAvailable_,
                &InputBuffer_,
                &availableOut,
                &outBuffer,
                nullptr);

            decompressedSize = size - availableOut;
            SubstreamFinished_ = (result == BROTLI_DECODER_RESULT_SUCCESS);

            if (result == BROTLI_DECODER_RESULT_ERROR) {
                BrotliDecoderErrorCode code = BrotliDecoderGetErrorCode(DecoderState_);
                ythrow yexception() << "Brotli decoder failed to decompress buffer: "
                                    << BrotliDecoderErrorString(code);
            } else if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
                Y_ABORT_UNLESS(availableOut != size,
                         "Buffer passed to read in Brotli decoder is too small");
                break;
            }
        } while (decompressedSize == 0 && result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && !InputExhausted_);

        if (!SubstreamFinished_ && decompressedSize == 0) {
            ythrow yexception() << "Input stream is incomplete";
        }

        return decompressedSize;
    }

private:
    IInputStream* Slave_;
    BrotliDecoderState* DecoderState_;

    const TBrotliDictionary* Dictionary_ = nullptr;

    bool SubstreamFinished_ = false;
    bool InputExhausted_ = false;
    const ui8* InputBuffer_ = nullptr;
    size_t InputAvailable_ = 0;

    unsigned char* TmpBuf() noexcept {
        return static_cast<unsigned char*>(AdditionalData());
    }

    size_t TmpBufLen() const noexcept {
        return AdditionalDataLength();
    }

    void InitDecoder() {
        DecoderState_ = BrotliDecoderCreateInstance(&TAllocator::Allocate, &TAllocator::Deallocate, nullptr);
        if (Dictionary_) {
            BrotliDecoderAttachDictionary(
                DecoderState_,
                BROTLI_SHARED_DICTIONARY_RAW,
                Dictionary_->GetSize(),
                Dictionary_->GetData());
        }
        if (!DecoderState_) {
            ythrow yexception() << "Brotli decoder initialization failed";
        }
    }

    void FreeDecoder() {
        BrotliDecoderDestroyInstance(DecoderState_);
    }

    void ResetState() {
        Y_ABORT_UNLESS(BrotliDecoderIsFinished(DecoderState_));
        FreeDecoder();
        InitDecoder();
    }
};

TBrotliDecompress::TBrotliDecompress(IInputStream* slave,
                                     size_t bufferSize,
                                     const TBrotliDictionary* dictionary)
    : Impl_(new (bufferSize) TImpl(slave, dictionary))
{
}

TBrotliDecompress::~TBrotliDecompress() = default;

size_t TBrotliDecompress::DoRead(void* buffer, size_t size) {
    return Impl_->Read(buffer, size);
}
