# STOCKOUT — GPU 자원 부족

## 증상

```
Error 503: ZONE_RESOURCE_POOL_EXHAUSTED_WITH_DETAILS
  NULL:0/NULL:0/NULL:0 (state:STOCKOUT, sub-state:STOCKOUT, resource type:compute)
```

`terraform apply` 또는 `gcloud compute instances start` 시 발생.

## 원인

해당 zone에 GPU(A100, L4, H100) 인스턴스 capacity가 일시적으로 없음. quota는 있어도 실제 물리 자원이 없으면 거부됨.

## 해결

다른 zone으로 변경. A100 가용 zone(2026년 5월 기준):

```bash
# 가용 zone 확인
gcloud compute accelerator-types list --filter="name:nvidia-tesla-a100" \
  --format="value(zone)" | sort -u
```

대표 zone:
- us-central1-a, us-central1-b, us-central1-c, us-central1-f
- us-east4-c
- asia-east1-a, asia-east1-b, asia-east1-c

`infra/terraform/terraform.tfvars` 수정:
```hcl
# zone = "us-central1-a"  # STOCKOUT
zone = "us-central1-b"    # 시도
```

이후 `terraform apply` 재실행.

## 예방

- 작업 마치면 `make vm-stop`이 아니라 `terraform destroy`로 자원 반납
- 인기 시간대(US 업무시간) 피하면 STOCKOUT 빈도 감소
- 정 안 되면 region 자체를 바꿔도 됨 (us-east4, europe-west4 등)

## 참고

STOCKOUT은 시간이 지나면 풀린다. 5-10분 후 재시도해도 됨. 다만 같은 zone에 계속 매여 있을 필요 없으니 보통 zone 변경이 빠름.
