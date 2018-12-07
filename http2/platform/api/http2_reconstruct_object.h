// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef QUICHE_HTTP2_PLATFORM_API_HTTP2_RECONSTRUCT_OBJECT_H_
#define QUICHE_HTTP2_PLATFORM_API_HTTP2_RECONSTRUCT_OBJECT_H_

#include <utility>

#include "net/third_party/quiche/src/http2/platform/impl/http2_reconstruct_object_impl.h"

namespace http2 {
namespace test {

class Http2Random;

// Reconstruct an object so that it is initialized as when it was first
// constructed. Runs the destructor to handle objects that might own resources,
// and runs the constructor with the provided arguments, if any.
template <class T, class... Args>
void Http2ReconstructObject(T* ptr, Http2Random* rng, Args&&... args) {
  Http2ReconstructObjectImpl(ptr, rng, std::forward<Args>(args)...);
}

// This version applies default-initialization to the object.
template <class T>
void Http2DefaultReconstructObject(T* ptr, Http2Random* rng) {
  Http2DefaultReconstructObjectImpl(ptr, rng);
}

}  // namespace test
}  // namespace http2

#endif  // QUICHE_HTTP2_PLATFORM_API_HTTP2_RECONSTRUCT_OBJECT_H_
