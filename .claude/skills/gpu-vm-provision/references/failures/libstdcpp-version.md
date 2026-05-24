# libstdc++ GLIBCXX_3.4.31 not found

## 증상

```
ERROR:  could not load library "/usr/lib/postgresql/16/lib/pg_cuvs.so":
        /lib/x86_64-linux-gnu/libstdc++.so.6: version `GLIBCXX_3.4.31' not found
        (required by /home/ubuntu/miniforge3/envs/cuvs_dev/lib/libcuvs.so)
```

또는 `GLIBCXX_3.4.32`, `GLIBCXX_3.4.30` 등 비슷한 패턴.

## 원인

- conda env의 cuvs_dev에는 GCC 14 기반 빌드된 libcuvs.so + libstdc++.so.6 (GLIBCXX_3.4.32+)
- Ubuntu 22.04 시스템은 GCC 11 기반 libstdc++.so.6 (GLIBCXX_3.4.30까지만)
- pg_cuvs.so의 rpath가 conda lib을 가리키고 있지만, PostgreSQL postmaster가 이미 시스템 libstdc++를 메모리에 로드한 상태라 충돌

ldd 결과는 conda lib을 잘 가리킨다:
```
libstdc++.so.6 => /home/ubuntu/miniforge3/envs/cuvs_dev/lib/libstdc++.so.6
```

하지만 actual 런타임에서 PG가 먼저 로드한 시스템 libstdc++이 우선됨.

## 해결 — 핵심 두 파일만 system 경로에 심볼릭 링크

`/usr/local/lib`은 `/lib/x86_64-linux-gnu`보다 ld.so 검색 우선순위가 높음.

```bash
ssh ubuntu@<IP> "
sudo ln -sf /home/ubuntu/miniforge3/envs/cuvs_dev/lib/libstdc++.so.6 /usr/local/lib/libstdc++.so.6
sudo ln -sf /home/ubuntu/miniforge3/envs/cuvs_dev/lib/libgcc_s.so.1 /usr/local/lib/libgcc_s.so.1
sudo ldconfig
sudo systemctl restart postgresql
"
```

확인:
```bash
sudo ldconfig -p | grep "libstdc++.so.6\|libgcc_s.so.1" | head -4
# /usr/local/lib/... 가 먼저, /lib/x86_64-linux-gnu/... 가 다음
```

## 왜 libgcc_s까지?

새 libstdc++는 새 libgcc_s 심볼을 요구한다. libstdc++만 새로 하고 libgcc_s를 시스템 거 그대로 두면 부분 호환성으로 다시 실패. 둘을 세트로 함께 새로 맞춰야 함.

## 왜 디렉터리 전체를 ldconfig에 넣으면 안 되나

이 함정은 다른 파일 [failures/ldconfig-disaster.md](ldconfig-disaster.md) 참조. 요약: conda env의 libssl.so.3, libdbus-1.so.3 등이 시스템 버전을 덮어써서 sshd/dbus가 시작 안 됨.

**핵심 원칙**: conda lib에서 시스템 경로로 가져올 때는 **반드시 파일 단위 심볼릭 링크**만. 디렉터리 등록 X.

## 추가 — 정적 링크는 부분만 해결

`SHLIB_LINK`에 `-Wl,-Bstatic -lstdc++ -Wl,-Bdynamic`을 넣으면 pg_cuvs.so 자체의 GLIBCXX 의존성은 해결되지만, **libcuvs.so의 동적 의존성은 여전히 conda libstdc++ 필요**. 즉:

- pg_cuvs.so: 정적 libstdc++ 포함됨 → 의존 없음
- libcuvs.so: 동적 의존 (rpath로 conda lib 찾음) → 시스템 libstdc++가 먼저 로드되면 충돌

따라서 정적 링크는 의존성을 *부분* 줄여줄 뿐, 시스템 libstdc++ 우선화 문제는 여전히 발생. 결국 위의 심볼릭 링크가 진짜 해결책.
