# Build-time errors (nvcc + PGXS)

cuvs_wrapper.cu 또는 PGXS 빌드에서 발생하는 컴파일/링크 오류 모음.

## 1. libcudacxx 헤더 경로

**증상**:
```
fatal error: cuda/std/__mdspan/concepts.h: No such file or directory
```

**원인**: cuVS 25.x+가 bundled libcudacxx를 `$CONDA_PREFIX/include/rapids/` 하위에 둠.

**해결**: Makefile에 `-I$(CUVS_PREFIX)/include/rapids` 추가.
```makefile
CUVS_RAPIDS_INCLUDE ?= $(CUVS_PREFIX)/include/rapids
PG_CPPFLAGS = -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -I./src
src/cuvs_wrapper.o: src/cuvs_wrapper.cu
	$(NVCC) ... -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -I./src -c $< -o $@
```

서버 빌드용 SERVER_CFLAGS에도 같이 추가.

## 2. RAFT_SYSTEM_LITTLE_ENDIAN undefined

**증상**:
```
raft/core/detail/mdspan_numpy_serializer.hpp(381): error: identifier
"RAFT_SYSTEM_LITTLE_ENDIAN" is undefined
```

**원인**: raft 26.x가 endian 매크로의 외부 정의를 요구.

**해결**: NVCC_FLAGS에 추가.
```makefile
NVCC_FLAGS ?= -O3 --compiler-options '-fPIC' -arch=$(CUDA_ARCH) -std=c++17 \
              -DRAFT_SYSTEM_LITTLE_ENDIAN=1
```

(x86_64는 항상 little endian이므로 1로 하드코딩 OK)

## 3. device_resources move 불가

**증상**:
```
error: function "raft::device_resources::device_resources(raft::device_resources &&)"
       cannot be referenced -- it is a deleted function
```

**원인**: cuVS 25.x+의 raft::device_resources는 move/copy 모두 deleted.

**해결**: 구조체 멤버로 값 보관 X. 함수마다 새로 만들기.
```cpp
// 24.x 패턴 — 안 됨
struct CuvsCagraIndexImpl {
    cuvs::neighbors::cagra::index<float, uint32_t> idx;
    raft::device_resources res;   // ← deleted move
    CuvsCagraIndexImpl(...&&i, ...&&r) : idx(std::move(i)), res(std::move(r)) {}
};

// 26.x 패턴 — 정상
struct CuvsCagraIndexImpl {
    cuvs::neighbors::cagra::index<float, uint32_t> idx;
    explicit CuvsCagraIndexImpl(cuvs::neighbors::cagra::index<float, uint32_t> &&i)
        : idx(std::move(i)) {}
};

// 각 함수에서 res 새로 생성
extern "C" int cuvs_cagra_search(CuvsCagraIndex index, ...) {
    raft::device_resources res;   // 매 호출 시 새로
    auto *impl = static_cast<CuvsCagraIndexImpl *>(index);
    // ... impl->idx + res 사용
}
```

비용: stream 새로 생성 → 작은 오버헤드. hot loop이 아니면 무시 OK.

## 4. cagra_serialize.hpp 사라짐

**증상**:
```
fatal error: cuvs/neighbors/cagra_serialize.hpp: No such file or directory
```

**원인**: 25.x+에서 `cagra_serialize.hpp`가 `cagra.hpp`로 통합됨.

**해결**: include 라인 제거.
```cpp
#include <cuvs/neighbors/cagra.hpp>  // serialize/deserialize 모두 여기에
// #include <cuvs/neighbors/cagra_serialize.hpp>  ← 제거
```

## 5. PGXS bitcode 규칙 누락

**증상**:
```
make: *** No rule to make target 'src/cuvs_wrapper.bc', needed by 'all'.  Stop.
```

**원인**: PGXS는 모든 OBJS에 대해 LLVM bitcode(.bc) 생성을 시도하지만 `.cu` 파일은 clang으로 비트코드 변환이 불가.

**해결**: stub bitcode 규칙 추가.
```makefile
src/cuvs_wrapper.bc: src/cuvs_wrapper.cu
	@echo 'void cuvs_wrapper_jit_stub(void){}' > /tmp/_cuvs_stub.c
	@clang-15 -emit-llvm -c /tmp/_cuvs_stub.c -o $@ 2>/dev/null \
		|| clang -emit-llvm -c /tmp/_cuvs_stub.c -o $@
	@rm /tmp/_cuvs_stub.c
```

JIT 인라이닝 대상이 아닌 .cu 파일에 대해 빈 stub bitcode를 만들어 PGXS를 만족시킨다.

## 6. clock_gettime / struct timeval 누락

**증상**:
```
error: implicit declaration of function 'clock_gettime'
error: 'CLOCK_MONOTONIC' undeclared
error: storage size of 'tv' isn't known (struct timeval)
```

**원인**: `-std=c11`만으로는 POSIX 함수가 노출되지 않음. `_POSIX_C_SOURCE` 매크로 정의 필요. 또한 `struct timeval`은 `<sys/time.h>` 헤더에 있음.

**해결**:
- Makefile SERVER_CFLAGS에 `-D_POSIX_C_SOURCE=200809L` 추가 (또는 `-std=gnu11`)
- cuvs_ipc.c, pg_cuvs_server.c에 `#include <sys/time.h>` 추가

## 7. Format `%ld` vs `int64_t` 경고

**증상**:
```
warning: format specifies type 'long' but the argument has type 'int64_t'
         (aka 'long long')
```

**원인**: 일부 환경에서 `int64_t == long long`인데 `%ld`는 `long`을 요구.

**해결**: 명시적 캐스트로 통일.
```c
fprintf(stderr, "n_vecs=%lld\n", (long long)e->n_vecs);
```

또는 `<inttypes.h>`의 `PRId64` 사용:
```c
#include <inttypes.h>
fprintf(stderr, "n_vecs=%" PRId64 "\n", e->n_vecs);
```

## 8. Linker: undefined reference to rmm::bad_alloc

**증상**:
```
src/cuvs_wrapper.o: undefined reference to symbol '_ZTVN3rmm9bad_allocE'
librmm.so: error adding symbols: DSO missing from command line
```

**원인**: libcuvs.so는 librmm.so에 transitive 의존. PGXS의 .so 링크는 `-Wl,--as-needed`로 transitive 의존을 자동 해결하지만, 데몬 바이너리(standalone) 직접 링크는 명시 필요.

**해결**: 서버 LDFLAGS에 `-lrmm` 추가.
```makefile
SERVER_LDFLAGS = -L$(CUVS_LIB) -lcuvs -lrmm -lcudart -lstdc++ \
                 -Wl,-rpath,$(CUVS_LIB) -lpthread -lrt
```

PGXS 측 `SHLIB_LINK`는 `-Wl,--as-needed` 기본 적용이라 -lrmm 명시 없이도 OK.
