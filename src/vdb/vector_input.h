#pragma once
#include "quanta_runtime.h"
#include <cstring>
#include <limits>

namespace Quanta {
// Borrows contiguous float32 storage for synchronous calls. Take() transfers or
// copies it into the asynchronous ingestion queue before the caller can mutate it.
class FloatInput {
    X::Value owner_;
    X3Buffer* buffer_ = nullptr;
    const float* data_ = nullptr;
    size_t size_ = 0;
    std::vector<float> owned_;

    template<class T> static float Read(const char* p) {
        T value;
        std::memcpy(&value, p, sizeof(value));
        return static_cast<float>(value);
    }
    void AppendList(const X::Value& value, unsigned depth) {
        if (depth > 32) throw X::Error("Vector nesting exceeds 32 dimensions");
        if (IsSequence(value)) {
            for (uint64_t i = 0; i < value.Size(); ++i) AppendList(value.Get(i), depth + 1);
        } else if (value.IsInt64() || value.IsDouble()) {
            owned_.push_back(static_cast<float>(value.ToDouble()));
        } else {
            throw X::Error("Vectors must contain numbers");
        }
    }
public:
    explicit FloatInput(const X::Value& value) : owner_(value) {
        if (value.IsList() || x3_value_object_kind(value.raw()) == X3_OBJECT_KIND_TUPLE) {
            AppendList(value, 0);
            data_ = owned_.data(); size_ = owned_.size();
            return;
        }
        X3TensorInfo tensor{};
        tensor.size = sizeof(tensor);
        if (x3_tensor_info(value.runtime(), value.raw(), &tensor) == X3_STATUS_OK) {
            if (tensor.symbolic || tensor.device_type != 0 || tensor.rank == UINT32_MAX)
                throw X::Error("Quanta requires an evaluated CPU tensor");
            const size_t width = (tensor.dtype == X3_TENSOR_FLOAT64 || tensor.dtype == X3_TENSOR_INT64) ? 8 : 4;
            size_ = 1;
            size_t stride = width;
            bool contiguous = true;
            for (uint32_t i = tensor.rank; i-- > 0;) {
                if (tensor.shape[i] < 0 || tensor.strides[i] < 0 ||
                    static_cast<uint64_t>(tensor.shape[i]) > std::numeric_limits<size_t>::max() / stride)
                    throw X::Error("Invalid tensor shape or strides");
                if (tensor.shape[i] > 1 && static_cast<uint64_t>(tensor.strides[i]) != stride) contiguous = false;
                size_ *= static_cast<size_t>(tensor.shape[i]);
                stride *= static_cast<size_t>(tensor.shape[i]);
                if (stride == 0) { size_ = 0; break; }
            }
            if (size_ && !tensor.data) throw X::Error("Tensor has no CPU storage");
            if (contiguous && tensor.dtype == X3_TENSOR_FLOAT32 &&
                reinterpret_cast<uintptr_t>(tensor.data) % alignof(float) == 0) {
                data_ = static_cast<const float*>(tensor.data);
                return;
            }
            owned_.resize(size_);
            for (size_t i = 0; i < size_; ++i) {
                size_t index = i, offset = 0;
                for (uint32_t d = tensor.rank; d-- > 0;) {
                    offset += (index % tensor.shape[d]) * tensor.strides[d];
                    index /= tensor.shape[d];
                }
                if (offset > tensor.byte_size || width > tensor.byte_size - offset)
                    throw X::Error("Tensor view exceeds storage");
                const char* p = static_cast<const char*>(tensor.data) + offset;
                switch (tensor.dtype) {
                case X3_TENSOR_FLOAT32: owned_[i] = Read<float>(p); break;
                case X3_TENSOR_FLOAT64: owned_[i] = Read<double>(p); break;
                case X3_TENSOR_INT32: owned_[i] = Read<int32_t>(p); break;
                case X3_TENSOR_INT64: owned_[i] = Read<int64_t>(p); break;
                default: throw X::Error("Unsupported tensor dtype");
                }
            }
        } else {
            X3BufferInfo info{};
            if (x3_buffer_acquire(value.runtime(), value.raw(), 0, &buffer_, &info) != X3_STATUS_OK) {
                if (IsSequence(value)) {
                    AppendList(value, 0);
                    data_ = owned_.data(); size_ = owned_.size();
                    return;
                }
                throw X::Error("Expected a numeric list, CPU tensor, or contiguous numeric buffer");
            }
            // A constructor failure must also release an acquired foreign buffer.
            try {
                std::string format = info.format ? info.format : "";
                if (!format.empty() && (format[0] == '@' || format[0] == '=')) format.erase(0, 1);
                if (!info.item_size || info.size % info.item_size ||
                    !((format == "f" && info.item_size == 4) || (format == "d" && info.item_size == 8)))
                    throw X::Error("Vector buffers must use native float32 or float64 format");
                size_ = static_cast<size_t>(info.size / info.item_size);
                if (format == "f" && reinterpret_cast<uintptr_t>(info.data) % alignof(float) == 0) {
                    data_ = static_cast<const float*>(info.data);
                    return;
                }
                owned_.resize(size_);
                for (size_t i = 0; i < size_; ++i) {
                    const char* p = static_cast<const char*>(info.data) + i * info.item_size;
                    owned_[i] = format == "f" ? Read<float>(p) : Read<double>(p);
                }
            } catch (...) { x3_buffer_release(buffer_); buffer_ = nullptr; throw; }
        }
        data_ = owned_.data();
    }
    ~FloatInput() { if (buffer_) x3_buffer_release(buffer_); }
    FloatInput(const FloatInput&) = delete;
    FloatInput& operator=(const FloatInput&) = delete;
    const float* data() const { return data_; }
    size_t size() const { return size_; }
    std::vector<float> Take() {
        if (!owned_.empty()) return std::move(owned_);
        return size_ ? std::vector<float>(data_, data_ + size_) : std::vector<float>{};
    }
};
}
