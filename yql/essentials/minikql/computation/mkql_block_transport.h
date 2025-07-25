#pragma once

#include <yql/essentials/minikql/mkql_node.h>

#include <yql/essentials/utils/chunked_buffer.h>

#include <arrow/datum.h>

#include <functional>
#include <memory>

namespace NKikimr::NMiniKQL {

class TBlockSerializerParams {
public:
    TBlockSerializerParams(arrow::MemoryPool* pool, TMaybe<ui8> minFillPercentage, bool shouldSerializeOffset)
        : Pool_(pool)
        , MinFillPercentage_(minFillPercentage)
        , ShouldSerializeOffset_(shouldSerializeOffset)
    {
    }

    arrow::MemoryPool* Pool() const {
        return Pool_;
    }

    TMaybe<ui8> MinFillPercentage() const {
        return MinFillPercentage_;
    }

    bool ShouldSerializeOffset() const {
        return ShouldSerializeOffset_;
    }

private:
    arrow::MemoryPool* Pool_;
    TMaybe<ui8> MinFillPercentage_;
    bool ShouldSerializeOffset_;
};

class IBlockSerializer {
public:
    virtual ~IBlockSerializer() = default;

    virtual size_t ArrayMetadataCount() const = 0;

    using TMetadataSink = std::function<void(ui64 meta)>;
    virtual void StoreMetadata(const arrow::ArrayData& data, const TMetadataSink& metaSink) const = 0;
    virtual void StoreArray(const arrow::ArrayData& data, NYql::TChunkedBuffer& dst) const = 0;
};

class IBlockDeserializer {
public:
    virtual ~IBlockDeserializer() = default;

    using TMetadataSource = std::function<ui64()>;
    virtual void LoadMetadata(const TMetadataSource& metaSource) = 0;

    virtual std::shared_ptr<arrow::ArrayData> LoadArray(NYql::TChunkedBuffer& src, ui64 blockLen, TMaybe<size_t> offset) = 0;
};

std::unique_ptr<IBlockSerializer> MakeBlockSerializer(const NYql::NUdf::ITypeInfoHelper& typeInfoHelper, const NYql::NUdf::TType* type, const TBlockSerializerParams& params);
std::unique_ptr<IBlockDeserializer> MakeBlockDeserializer(const NYql::NUdf::ITypeInfoHelper& typeInfoHelper, const NYql::NUdf::TType* type, const TBlockSerializerParams& params);

}
