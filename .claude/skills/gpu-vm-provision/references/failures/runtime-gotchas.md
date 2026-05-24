# Runtime Gotchas

extension은 로드되지만 SQL/IPC 레벨에서 발생하는 오류.

## 1. pgvector 함수명 오해 (cosine_distance)

**증상**:
```
ERROR:  function vector_cosine_distance(vector, vector) does not exist
```
(CREATE OPERATOR CLASS 또는 CREATE EXTENSION 시)

**원인**: pgvector의 실제 함수명은 `cosine_distance`다. `vector_l2_squared_distance` 와 `vector_negative_inner_product`에는 `vector_` 접두사가 있지만, **cosine은 없다**.

**해결**: `sql/pg_cuvs--0.1.0.sql`에서:
```sql
CREATE OPERATOR CLASS vector_cosine_ops
FOR TYPE vector USING cagra AS
    OPERATOR 1 <=> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 cosine_distance(vector, vector);  -- vector_ 접두사 없음
```

확인:
```sql
SELECT proname FROM pg_proc WHERE proname ~ 'cosine|l2|inner' ORDER BY proname;
-- cosine_distance, vector_l2_squared_distance, vector_negative_inner_product
```

## 2. "required extension vector is not installed"

**증상**:
```
ERROR:  required extension "vector" is not installed
HINT:  Use CREATE EXTENSION ... CASCADE to install required extensions too.
```

**원인**: `pg_cuvs.control`에 `requires = 'vector'` 명시되어 있어 CREATE EXTENSION pg_cuvs 전에 vector 먼저 설치 필요.

**해결**:
```sql
CREATE EXTENSION IF NOT EXISTS vector;  -- pgvector 먼저
CREATE EXTENSION pg_cuvs;
```
또는:
```sql
CREATE EXTENSION pg_cuvs CASCADE;  -- vector 자동 함께 설치
```

DROP 시 주의: `DROP EXTENSION pg_cuvs CASCADE`는 pgvector까지 제거.

## 3. systemd: status 209/STDOUT

**증상**:
```
pg-cuvs-server.service: Main process exited, code=exited, status=209/STDOUT
Failed to set up standard output: Permission denied
```

**원인**: systemd unit에 `StandardOutput=append:/tmp/...`로 파일 리다이렉트 지정 시 권한 거부. systemd가 보안상 사용자 디렉터리 파일에 직접 append 못 함.

**해결**: 파일 리다이렉트 제거. 기본 journal 사용.
```ini
[Service]
Type=simple
User=ubuntu
Group=ubuntu
ExecStart=/usr/lib/postgresql/16/bin/pg_cuvs_server ...
# Standard{Output,Error}= 라인 제거
```

로그 조회:
```bash
sudo journalctl -u pg-cuvs-server -n 30 --no-pager
```

## 4. shm_open errno 13 (Permission denied)

**증상** (daemon journal):
```
[handle_build] shm_open(/pg_cuvs_<pid>_<seq>)...
[handle_build] shm_open FAILED errno=13 (Permission denied)
```

**원인**: postgres 유저가 만든 `/dev/shm/pg_cuvs_...` 파일이 umask 0077의 영향으로 실제로는 0600 권한이다. shm_open의 3번째 인자 `0666`은 umask와 AND 연산돼서 무력화된다. daemon은 ubuntu 유저로 실행되어 다른 유저의 0600 파일을 못 읽음.

**해결**: shm_open 직후 `fchmod(fd, 0666)`로 umask 명시적 우회.
```c
int fd = shm_open(shm_key, O_CREAT | O_RDWR, 0666);
if (fd < 0) return -1;
fchmod(fd, 0666);   // 핵심: umask 영향 무력화
```

## 5. Client returns "status 1" but daemon journal shows success

**증상**:
- 클라이언트: `WARNING: pg_cuvs: daemon returned status 1 during BUILD`
- 데몬 저널: `built index ... (N vecs)` (정상 완료)

**원인**: 클라이언트 소켓의 SO_RCVTIMEO가 1초인데 cuvs_cagra_build는 수 초 걸림. 클라이언트는 timeout으로 status 1 반환, 데몬은 나중에 OK 응답하지만 클라이언트는 이미 떠남.

**해결**: BUILD 경로 전용 긴 timeout 함수 분리.
```c
static int uds_connect_ex(const char *socket_path, int recv_timeout_sec) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    // ...
    struct timeval tv = {.tv_sec = recv_timeout_sec, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    // ...
}

static int uds_connect(const char *socket_path) {
    return uds_connect_ex(socket_path, 30);  // SEARCH 기본 30초
}

// cuvs_ipc_build:
sock = uds_connect_ex(socket_path, 600);  // BUILD 10분
```

## 6. /home/ubuntu traversal denied (postgres 유저)

**증상**: extension 로드 시 `cannot open shared object file: No such file or directory` (libcuvs.so 또는 다른 conda lib에 대해)

**원인**: pg_cuvs.so에 박힌 rpath가 `/home/ubuntu/miniforge3/envs/cuvs_dev/lib`인데, postgres 유저가 `/home/ubuntu` 디렉터리를 traverse(`+x`)할 권한이 없음. ldd 자체는 ubuntu 유저로 보면 정상이라 헷갈리기 쉬움.

**해결**: 부모 디렉터리 traverse 권한만 풀기.
```bash
sudo chmod o+x /home/ubuntu
sudo chmod o+x /home/ubuntu/miniforge3
sudo chmod o+x /home/ubuntu/miniforge3/envs
sudo chmod o+x /home/ubuntu/miniforge3/envs/cuvs_dev
sudo chmod o+x /home/ubuntu/miniforge3/envs/cuvs_dev/lib
```

read 권한(+r)은 필요 없음. traverse만으로 충분.

## 7. SSH 끊김 — 데몬이 controlling terminal 잡음

**증상**: `ssh ubuntu@<IP> '/path/to/pg_cuvs_server ... &'` 명령 후 SSH가 즉시 끊김. exit code 255.

**원인**: bash `&`만으로 백그라운드 처리해도 데몬이 controlling tty와 stdio를 잡고 있어 SSH 세션과 함께 끊김.

**해결**: systemd unit으로 관리. 직접 SSH로 데몬을 백그라운드 띄우는 패턴은 지양.

급할 때 임시 방편 (테스트용):
```bash
ssh ubuntu@<IP> "nohup /path/to/pg_cuvs_server ... > /tmp/log 2>&1 </dev/null &"
# nohup + < /dev/null 둘 다 필요
```

장기적으로는 `references/quick-start.md` Step 8의 systemd unit 사용.

## 8. Database 새로 만든 후 cuvs.index_dir 기본값 불일치

**증상**: BUILD 시 `[handle_build] got index_dir=/var/lib/postgresql/16/main/cuvs_indexes`로 오는데 데몬은 `/tmp/cuvs_indexes` 사용. 결국 daemon이 PG의 데이터 디렉터리에 쓰려다 권한 거부.

**원인**: pg_cuvs.c의 default가 `$DataDir/cuvs_indexes`. ubuntu 유저로 실행되는 데몬은 PG의 `/var/lib/postgresql/16/main/` 하위에 쓸 권한이 없음.

**해결**: 세션마다 `SET cuvs.index_dir = '/tmp/cuvs_indexes'` 또는 postgresql.conf에 영구 설정.
```sql
ALTER SYSTEM SET cuvs.index_dir = '/tmp/cuvs_indexes';
SELECT pg_reload_conf();
```
