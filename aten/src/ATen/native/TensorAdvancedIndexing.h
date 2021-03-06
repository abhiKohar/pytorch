#pragma once

// Indexing tensors by by tensors

#include <ATen/ATen.h>
#include <ATen/native/DispatchStub.h>

namespace at {
  struct TensorIterator;
}

namespace at { namespace native {

using index_fn = void(*)(TensorIterator &, IntArrayRef indexed_sizes, IntArrayRef indexed_strides);
using index_put_fn = void(*)(TensorIterator &, IntArrayRef indexed_sizes, IntArrayRef indexed_strides, bool accumulate);
using index_put_accum_fn = void(*)(Tensor &, TensorList , const Tensor &, bool unsafe);

DECLARE_DISPATCH(index_fn, index_stub);
DECLARE_DISPATCH(index_put_fn, index_put_stub);
DECLARE_DISPATCH(index_put_accum_fn, index_put_accum_stub);

}} // namespace at::native
