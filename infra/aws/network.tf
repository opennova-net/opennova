# ---------------------------------------------------------------------------
# The VPC
# ---------------------------------------------------------------------------
#
# 10.44.0.0/16 (var.vpc_cidr) in two availability zones:
#   public a  10.44.0.0/24   the service host and the game machines
#   public b  10.44.1.0/24   spare capacity for machines
#   private a 10.44.16.0/24  the database
#   private b 10.44.17.0/24  the database subnet group's second zone
# The public subnets route to the internet gateway and hand out public
# addresses; the private ones have the VPC's local route alone (no NAT: nothing
# there talks to the internet).
#
# Public a is the subnet the single-file layout made (aws_subnet.auto[0]): its
# range and zone stay what they were, so the move replaces nothing. Pin the zones
# (var.azs) to keep it so if the region's list of zones ever changes order.

data "aws_availability_zones" "available" {
  state = "available"
}

locals {
  public_subnets = {
    a = { cidr = cidrsubnet(var.vpc_cidr, 8, 0), az = local.azs[0], name = "${local.name}-subnet" }
    b = { cidr = cidrsubnet(var.vpc_cidr, 8, 1), az = local.azs[1], name = "${local.name}-public-b" }
  }
  private_subnets = {
    a = { cidr = cidrsubnet(var.vpc_cidr, 8, 16), az = local.azs[0], name = "${local.name}-private-a" }
    b = { cidr = cidrsubnet(var.vpc_cidr, 8, 17), az = local.azs[1], name = "${local.name}-private-b" }
  }
}

resource "aws_vpc" "main" {
  cidr_block           = var.vpc_cidr
  enable_dns_support   = true
  enable_dns_hostnames = true

  tags = merge(var.tags, {
    Name = "${local.name}-vpc"
  })
}

resource "aws_internet_gateway" "main" {
  vpc_id = aws_vpc.main.id

  tags = merge(var.tags, {
    Name = "${local.name}-igw"
  })
}

resource "aws_subnet" "public" {
  for_each = local.public_subnets

  vpc_id                  = aws_vpc.main.id
  cidr_block              = each.value.cidr
  availability_zone       = each.value.az
  map_public_ip_on_launch = true

  tags = merge(var.tags, {
    Name = each.value.name
  })
}

resource "aws_route_table" "public" {
  vpc_id = aws_vpc.main.id

  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.main.id
  }

  tags = merge(var.tags, {
    Name = "${local.name}-rt"
  })
}

resource "aws_route_table_association" "public" {
  for_each = local.public_subnets

  subnet_id      = aws_subnet.public[each.key].id
  route_table_id = aws_route_table.public.id
}

resource "aws_subnet" "private" {
  for_each = local.private_subnets

  vpc_id                  = aws_vpc.main.id
  cidr_block              = each.value.cidr
  availability_zone       = each.value.az
  map_public_ip_on_launch = false

  tags = merge(var.tags, {
    Name = each.value.name
  })
}

# The local route alone: no internet gateway, no NAT. `route = []` makes it
# explicit (and removes any route added by hand).
resource "aws_route_table" "private" {
  vpc_id = aws_vpc.main.id
  route  = []

  tags = merge(var.tags, {
    Name = "${local.name}-private"
  })
}

resource "aws_route_table_association" "private" {
  for_each = local.private_subnets

  subnet_id      = aws_subnet.private[each.key].id
  route_table_id = aws_route_table.private.id
}
