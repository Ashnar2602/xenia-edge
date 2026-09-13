/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cfloat>
#include <climits>

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

TEST_CASE("VECTOR_MAX_I8_SIGNED", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorMax(LoadVR(b, 4), LoadVR(b, 5), INT8_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[5] = vec128b(-100, 1, 100, -3, 4, -5, 60, 7, -80, 9, 10,
                            INT8_MIN, INT8_MAX, 13, 2, 0);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(0, 1, 100, 3, 4, 5, 60, 7, 8, 9, 10, 11,
                                  INT8_MAX, 13, 14, 15));
      });
}

TEST_CASE("VECTOR_MAX_I8_UNSIGNED", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.VectorMax(LoadVR(b, 4), LoadVR(b, 5), INT8_TYPE,
                        ARITHMETIC_UNSIGNED));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[5] = vec128b(-100, 1, 100, -3, 4, -5, 60, 7, -80, 9, 10,
                            INT8_MIN, INT8_MAX, 13, 2, 0);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(-100, 1, 100, -3, 4, -5, 60, 7, -80, 9, 10,
                                  INT8_MIN, INT8_MAX, 13, 14, 15));
      });
}

TEST_CASE("VECTOR_MAX_I16_SIGNED", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorMax(LoadVR(b, 4), LoadVR(b, 5), INT16_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128s(0, 1, 2, 3, 4, 5, -6000, 7);
        ctx->v[5] = vec128s(-1000, 1, -2000, 3, 4, SHRT_MAX, 6, 0);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128s(0, 1, 2, 3, 4, SHRT_MAX, 6, 7));
      });
}

TEST_CASE("VECTOR_MAX_I16_UNSIGNED", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.VectorMax(LoadVR(b, 4), LoadVR(b, 5), INT16_TYPE,
                        ARITHMETIC_UNSIGNED));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128s(0, 1, 2, 3, 4, 5, -6000, 7);
        ctx->v[5] = vec128s(-1000, 1, -2000, 3, 4, USHRT_MAX, 6, 0);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128s(-1000, 1, -2000, 3, 4, USHRT_MAX, -6000, 7));
      });
}

TEST_CASE("VECTOR_MAX_I32_SIGNED", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorMax(LoadVR(b, 4), LoadVR(b, 5), INT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0, 1, 123, 3);
        ctx->v[5] = vec128i(-1000000, 0, INT_MAX, 0);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128i(0, 1, INT_MAX, 3));
      });
}

TEST_CASE("VECTOR_MAX_I32_UNSIGNED", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.VectorMax(LoadVR(b, 4), LoadVR(b, 5), INT32_TYPE,
                        ARITHMETIC_UNSIGNED));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0, 1, 123, 3);
        ctx->v[5] = vec128i(-1000000, 0, UINT_MAX, 0);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128i(-1000000, 1, UINT_MAX, 3));
      });
}

TEST_CASE("VECTOR_MAX_CONSTANT_OPERANDS_MATCH_REGISTERS", "[instr]") {
  const vec128_t a = vec128b(0x00, 0x7F, 0x80, 0xFF, 0x01, 0xFE, 0x12, 0x34,
                             0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x0F, 0x80);
  const vec128_t c = vec128b(0xFF, 0x80, 0x7F, 0x00, 0xFE, 0x01, 0x34, 0x12,
                             0x9A, 0x56, 0x78, 0xF0, 0xDE, 0xBC, 0x80, 0x0F);
  for (TypeName part : {INT8_TYPE, INT16_TYPE, INT32_TYPE}) {
    for (uint32_t flags : {uint32_t(0), uint32_t(ARITHMETIC_UNSIGNED)}) {
      RequireConstantOperandsMatchRegisters(
          a, c, [part, flags](HIRBuilder& b, Value* x, Value* y) {
            return b.VectorMax(x, y, part, flags);
          });
    }
  }
}
