output "instance_name" {
  value = google_compute_instance.pg_cuvs_dev.name
}

output "external_ip" {
  value       = google_compute_instance.pg_cuvs_dev.network_interface[0].access_config[0].nat_ip
  description = "Use in .env.gpu: GCP_VM=ubuntu@<this IP>"
}

output "zone" {
  value = google_compute_instance.pg_cuvs_dev.zone
}

output "env_gpu_snippet" {
  description = "Paste into .env.gpu"
  value       = <<-EOT
    GCP_VM=ubuntu@${google_compute_instance.pg_cuvs_dev.network_interface[0].access_config[0].nat_ip}
    GCP_INSTANCE=${google_compute_instance.pg_cuvs_dev.name}
    GCP_ZONE=${google_compute_instance.pg_cuvs_dev.zone}
    GCP_PROJECT=${var.project_id}
    CONDA_ENV=cuvs_dev
    CUDA_ARCH=sm_80
  EOT
}
