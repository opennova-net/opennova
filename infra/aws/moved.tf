# Address moves from the single-file layout (main.tf, one optional
# bring-your-own VPC). Each keeps its object: the plan shows them as moved, never
# as a destroy and a create.

# The single-host stack's Elastic IP (aws_eip.server[0], prod's 54.162.79.147):
# retail clients reach the service by it, so it must outlive the stack that
# made it. Terraform forgets it here without releasing it; prod's
# service_eip_allocation_id then adopts it by allocation id (service.tf), and
# no later plan can release it. (`infra state-rm 'aws_eip.server[0]'` before the
# cutover's apply does the same by hand; DEPLOY.md, "Moving from the
# single-host stack".)
removed {
  from = aws_eip.server

  lifecycle {
    destroy = false
  }
}

# The network: the auto-created VPC and its one subnet are now the VPC and its
# public subnet a.
moved {
  from = aws_vpc.auto[0]
  to   = aws_vpc.main
}

moved {
  from = aws_internet_gateway.auto[0]
  to   = aws_internet_gateway.main
}

moved {
  from = aws_subnet.auto[0]
  to   = aws_subnet.public["a"]
}

moved {
  from = aws_route_table.auto[0]
  to   = aws_route_table.public
}

moved {
  from = aws_route_table_association.auto[0]
  to   = aws_route_table_association.public["a"]
}

# The backups bucket is the operations bucket (ops_bucket.tf).
moved {
  from = aws_s3_bucket.backups
  to   = aws_s3_bucket.ops
}

moved {
  from = aws_s3_bucket_versioning.backups
  to   = aws_s3_bucket_versioning.ops
}

moved {
  from = aws_s3_bucket_server_side_encryption_configuration.backups
  to   = aws_s3_bucket_server_side_encryption_configuration.ops
}

moved {
  from = aws_s3_bucket_lifecycle_configuration.backups
  to   = aws_s3_bucket_lifecycle_configuration.ops
}

moved {
  from = aws_s3_bucket_public_access_block.backups
  to   = aws_s3_bucket_public_access_block.ops
}
