# ---------------------------------------------------------------------------
# The runners' network
# ---------------------------------------------------------------------------
#
# One VPC with a public subnet in each of three zones (more zones, more spot
# capacity to pick from) and an internet gateway. RunsOn's launch templates
# give each runner a public address; nothing reaches a runner from outside (no
# SSH), and there is no NAT gateway to pay for.

data "aws_availability_zones" "available" {
  state = "available"
}

resource "aws_vpc" "ci" {
  cidr_block           = "10.17.0.0/16"
  enable_dns_support   = true
  enable_dns_hostnames = true

  tags = {
    Name = "opennova-ci"
  }
}

resource "aws_internet_gateway" "ci" {
  vpc_id = aws_vpc.ci.id

  tags = {
    Name = "opennova-ci"
  }
}

resource "aws_subnet" "public" {
  count             = 3
  vpc_id            = aws_vpc.ci.id
  availability_zone = data.aws_availability_zones.available.names[count.index]
  cidr_block        = cidrsubnet(aws_vpc.ci.cidr_block, 4, count.index)

  tags = {
    Name = "opennova-ci-public-${data.aws_availability_zones.available.names[count.index]}"
  }
}

resource "aws_route_table" "public" {
  vpc_id = aws_vpc.ci.id

  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.ci.id
  }

  tags = {
    Name = "opennova-ci-public"
  }
}

resource "aws_route_table_association" "public" {
  count          = length(aws_subnet.public)
  subnet_id      = aws_subnet.public[count.index].id
  route_table_id = aws_route_table.public.id
}
