variable "project_id" {
  description = "GCP project ID"
  type        = string
}

variable "region" {
  description = "GCP region (asia-east1 = closest L4 region to Korea)"
  type        = string
  default     = "asia-east1"
}

variable "zone" {
  description = "GCP zone (L4 availability)"
  type        = string
  default     = "asia-east1-a"
}

variable "instance_name" {
  description = "GPU VM instance name"
  type        = string
  default     = "pg-cuvs-dev"
}

variable "machine_type" {
  description = "Machine type — a2-highgpu-1g has 1x A100-40GB + 12 vCPU + 85GB RAM"
  type        = string
  default     = "a2-highgpu-1g"
}

variable "accelerator_type" {
  description = "GPU accelerator type (nvidia-tesla-a100 for A100-40GB, nvidia-a100-80gb, nvidia-h100-80gb, nvidia-l4)"
  type        = string
  default     = "nvidia-tesla-a100"
}

variable "disk_size_gb" {
  description = "Boot disk size in GB (conda envs + PG data + index files)"
  type        = number
  default     = 100
}

variable "ssh_user" {
  description = "SSH username for Makefile gpu-* targets"
  type        = string
  default     = "ubuntu"
}

variable "ssh_pub_key_path" {
  description = "Path to SSH public key"
  type        = string
  default     = "~/.ssh/id_rsa.pub"
}

variable "gpu_count" {
  description = "Number of GPUs for the multi-GPU instance (2/4/8). Set >1 to create pg-cuvs-dev-mgpu."
  type        = number
  default     = 1
}

variable "mgpu_zone" {
  description = "Zone for the multi-GPU instance (may differ from dev VM zone due to capacity)"
  type        = string
  default     = "us-central1-f"
}

variable "preemptible" {
  description = "Use preemptible VM to reduce cost (stops after 24h)"
  type        = bool
  default     = false
}
