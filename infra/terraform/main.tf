terraform {
  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 5.0"
    }
  }
}

provider "google" {
  project = var.project_id
  region  = var.region
  zone    = var.zone
}

locals {
  ssh_metadata = {
    ssh-keys = "${var.ssh_user}:${file(var.ssh_pub_key_path)}"
  }
}

# Firewall: SSH only from outside
resource "google_compute_firewall" "ssh" {
  name    = "${var.instance_name}-allow-ssh"
  network = "default"

  allow {
    protocol = "tcp"
    ports    = ["22"]
  }

  source_ranges = ["0.0.0.0/0"]
  target_tags   = ["pg-cuvs-dev"]
}

# GPU VM: NVIDIA L4 (g2-standard-4)
resource "google_compute_instance" "pg_cuvs_dev" {
  name         = var.instance_name
  machine_type = var.machine_type
  tags         = ["pg-cuvs-dev"]

  # GPU (A100 default; single-GPU dev instance)
  guest_accelerator {
    type  = var.accelerator_type
    count = 1
  }

  scheduling {
    on_host_maintenance = "TERMINATE"  # required for GPU instances
    automatic_restart   = !var.preemptible
    preemptible         = var.preemptible
  }

  boot_disk {
    initialize_params {
      # Ubuntu 22.04 LTS — CUDA 12 driver support confirmed
      image = "ubuntu-os-cloud/ubuntu-2204-lts"
      size  = var.disk_size_gb
      type  = "pd-ssd"
    }
  }

  network_interface {
    network = "default"
    access_config {}  # ephemeral external IP
  }

  metadata = local.ssh_metadata

  # Startup script: NVIDIA driver + CUDA + Miniforge + cuVS + PG16 + pgvector
  metadata_startup_script = file("${path.module}/scripts/install_gpu_env.sh")

  allow_stopping_for_update = true
}

# Multi-GPU VM for Phase 3E testing (optional — set gpu_count > 1 to create)
resource "google_compute_instance" "pg_cuvs_mgpu" {
  count        = var.gpu_count > 1 ? 1 : 0
  name         = "${var.instance_name}-mgpu"
  machine_type = "a2-highgpu-${var.gpu_count}g"
  tags         = ["pg-cuvs-dev"]

  guest_accelerator {
    type  = var.accelerator_type
    count = var.gpu_count
  }

  scheduling {
    on_host_maintenance = "TERMINATE"
    automatic_restart   = !var.preemptible
    preemptible         = var.preemptible
  }

  boot_disk {
    initialize_params {
      image = "ubuntu-os-cloud/ubuntu-2204-lts"
      size  = var.disk_size_gb
      type  = "pd-ssd"
    }
  }

  network_interface {
    network = "default"
    access_config {}
  }

  metadata = local.ssh_metadata

  metadata_startup_script = file("${path.module}/scripts/install_gpu_env.sh")

  allow_stopping_for_update = true
}
