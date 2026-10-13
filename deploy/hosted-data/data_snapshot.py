#!/usr/bin/env python3
"""The hosted game servers' game data, once, as a private encrypted EBS snapshot.

ADR 0053; DEPLOY.md "Hosted game servers". The deploy toolbox's `hosted data-snapshot`
streams a tar of the Joint Operations files opennova-serve reads into this program's
standard input. It

  1. extracts them into a staging directory (regular files and directories alone);
  2. checks the set (the three base archives, Escalation's pair and its version.txt);
  3. builds an ext4 file system image labelled opennova-jo holding them (no journal:
     every machine mounts it read-only), checked with e2fsck;
  4. writes the image's non-zero blocks straight into a new snapshot through the EBS
     direct APIs (StartSnapshot, PutSnapshotBlock, CompleteSnapshot), encrypted with
     the account's default EBS key, tagged opennova:hosted-data=1 and the deployment.
     That key must be the AWS-managed aws/ebs one, which infra/aws/hosted.tf's launch
     policy relies on (it grants no KMS action): another is refused before a block is
     written, and a key it cannot read (no kms:DescribeKey) is a warning;
  5. waits until EC2 reports the snapshot completed, and prints its id on standard
     output, the one line there (everything else goes to standard error).

No machine, no bucket, no SSH: the only AWS resource it makes is the snapshot, and on
any failure or interrupt before the snapshot completed it deletes that snapshot. The
staging directory and the image live in this container alone (the toolbox runs it with
--rm). The credentials and the region come from the environment (AWS_ACCESS_KEY_ID,
AWS_SECRET_ACCESS_KEY, AWS_REGION), as the toolbox's `aws` passes them.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import hashlib
import os
import signal
import subprocess
import sys
import tarfile
import tempfile
import uuid

import boto3
from botocore.config import Config

LABEL = "opennova-jo"
# What opennova-serve mounts: the retail boot table and Escalation's pair, and the
# version.txt whose checksum the session advertises (engine/base/vfs/vfs.h).
REQUIRED = [
    "resource.pff",
    "localres.pff",
    "language.pff",
    "expansion/jox01/jox01.pff",
    "expansion/jox01/jox01l.pff",
    "expansion/jox01/version.txt",
]
GIB = 1 << 30
IN_FLIGHT = 64
WORKERS = 16


def log(message: str) -> None:
    print(f"[hosted-data] {message}", file=sys.stderr, flush=True)


def interrupted(signum, frame):  # noqa: ARG001 - the signal module's signature
    raise KeyboardInterrupt(f"signal {signum}")


def extract(stage: str) -> list[tuple[str, int]]:
    """The tar on stdin into `stage`: its files and their sizes."""
    with tarfile.open(fileobj=sys.stdin.buffer, mode="r|") as tar:
        for member in tar:
            if not (member.isfile() or member.isdir()):
                raise SystemExit(f"the tar holds {member.name!r}, which is no file or directory")
            tar.extract(member, stage, filter="data")
    files = []
    for root, dirs, names in os.walk(stage):
        for d in dirs:
            os.chmod(os.path.join(root, d), 0o755)
        for name in names:
            path = os.path.join(root, name)
            os.chmod(path, 0o644)
            files.append((os.path.relpath(path, stage).replace(os.sep, "/"), os.path.getsize(path)))
    return sorted(files)


def check(files: list[tuple[str, int]]) -> None:
    present = {name.lower() for name, _ in files}
    missing = [name for name in REQUIRED if name not in present]
    if missing:
        raise SystemExit("the data lacks " + ", ".join(missing))
    empty = [name for name, size in files if size == 0 and name.lower().endswith(".pff")]
    if empty:
        raise SystemExit("empty archives: " + ", ".join(empty))


def build_image(stage: str, image: str, data_bytes: int) -> int:
    """An ext4 image of `stage` at `image`; its size in GiB."""
    size_gib = max(1, -(-(data_bytes + data_bytes // 10 + (128 << 20)) // GIB))
    with open(image, "wb") as f:
        f.truncate(size_gib * GIB)
    subprocess.run(
        ["mkfs.ext4", "-q", "-F", "-L", LABEL, "-O", "^has_journal", "-m", "0",
         "-E", "root_owner=0:0", "-d", stage, image],
        check=True)
    subprocess.run(["e2fsck", "-fn", image], check=True, stdout=sys.stderr)
    return size_gib


def upload(ebs, image: str, snapshot_id: str, block_size: int) -> int:
    """The image's non-zero blocks into the snapshot: how many."""
    zero = bytes(block_size)

    def put(index: int, block: bytes) -> None:
        digest = base64.b64encode(hashlib.sha256(block).digest()).decode()
        ebs.put_snapshot_block(SnapshotId=snapshot_id, BlockIndex=index, BlockData=block,
                               DataLength=len(block), Checksum=digest, ChecksumAlgorithm="SHA256")

    changed = 0
    pending: set[concurrent.futures.Future] = set()
    with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool, open(image, "rb") as f:
        index = 0
        while True:
            block = f.read(block_size)
            if not block:
                break
            if len(block) != block_size:
                raise SystemExit(f"block {index} is short ({len(block)} bytes)")
            if block != zero:
                pending.add(pool.submit(put, index, block))
                changed += 1
                if len(pending) >= IN_FLIGHT:
                    done, pending = concurrent.futures.wait(pending, return_when=concurrent.futures.FIRST_COMPLETED)
                    for future in done:
                        future.result()
            index += 1
            if index % 512 == 0:
                log(f"  {index * block_size >> 20} MiB read, {changed} blocks written")
        for future in concurrent.futures.as_completed(pending):
            future.result()
    return changed


def check_key(kms, key_arn: str | None) -> None:
    """Refuses a snapshot encrypted with any key but the AWS-managed aws/ebs one.

    hosted.tf's service policy grants no KMS action: a launch from a snapshot under
    a customer-managed key would fail. Reading the aws/ebs key needs kms:DescribeKey;
    without it the check is a warning, not a refusal.
    """
    if not key_arn:
        log("warning: StartSnapshot named no key; check the snapshot's key is aws/ebs")
        return
    try:
        managed = kms.describe_key(KeyId="alias/aws/ebs")["KeyMetadata"]["Arn"]
    except Exception as error:  # noqa: BLE001 - any failure to read it is the warning
        log(f"warning: could not read the aws/ebs key to compare ({error}); the snapshot's key is "
            f"{key_arn}: if that is a customer-managed key, launches need a KMS grant (infra/aws/hosted.tf)")
        return
    if key_arn != managed:
        raise SystemExit(
            f"the snapshot's key {key_arn} is not the AWS-managed aws/ebs key ({managed}): the account's "
            "default EBS key is another, and hosted.tf's launch policy grants no KMS action for it. "
            "Reset the default (aws ec2 reset-ebs-default-kms-key-id) or extend hosted.tf first")
    log("the snapshot's key is aws/ebs")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--deployment", required=True, help="the opennova:deployment tag")
    parser.add_argument("--region", default=os.environ.get("AWS_REGION") or os.environ.get("AWS_DEFAULT_REGION"))
    args = parser.parse_args()
    if not args.region:
        raise SystemExit("no region (AWS_REGION)")
    signal.signal(signal.SIGTERM, interrupted)

    config = Config(retries={"max_attempts": 8, "mode": "standard"})
    ebs = boto3.client("ebs", region_name=args.region, config=config)
    ec2 = boto3.client("ec2", region_name=args.region, config=config)
    kms = boto3.client("kms", region_name=args.region, config=config)

    with tempfile.TemporaryDirectory(prefix="opennova-hosted-data-") as work:
        stage = os.path.join(work, "stage")
        os.mkdir(stage)
        files = extract(stage)
        check(files)
        data_bytes = sum(size for _, size in files)
        for name, size in files:
            log(f"  {name}  {size:,} bytes")
        log(f"{len(files)} files, {data_bytes:,} bytes")
        image = os.path.join(work, "opennova-jo.ext4")
        size_gib = build_image(stage, image, data_bytes)
        log(f"ext4 image labelled {LABEL}, {size_gib} GiB")

        snapshot_id = None
        completed = False
        try:
            started = ebs.start_snapshot(
                VolumeSize=size_gib,
                Encrypted=True,
                Timeout=120,
                ClientToken=str(uuid.uuid4()),
                Description="OpenNova hosted game servers' Joint Operations data (private; ADR 0053)",
                Tags=[
                    {"Key": "Name", "Value": f"{args.deployment}-hosted-data"},
                    {"Key": "opennova:hosted-data", "Value": "1"},
                    {"Key": "opennova:deployment", "Value": args.deployment},
                ])
            snapshot_id = started["SnapshotId"]
            log(f"snapshot {snapshot_id} started (key {started.get('KmsKeyArn', 'the default EBS key')})")
            check_key(kms, started.get("KmsKeyArn"))
            changed = upload(ebs, image, snapshot_id, started["BlockSize"])
            ebs.complete_snapshot(SnapshotId=snapshot_id, ChangedBlocksCount=changed)
            log(f"{changed} blocks written; waiting for EC2 to complete {snapshot_id}")
            ec2.get_waiter("snapshot_completed").wait(
                SnapshotIds=[snapshot_id], WaiterConfig={"Delay": 15, "MaxAttempts": 480})
            described = ec2.describe_snapshots(SnapshotIds=[snapshot_id])["Snapshots"][0]
            if not described.get("Encrypted"):
                raise SystemExit(f"{snapshot_id} completed unencrypted")
            completed = True
        finally:
            if snapshot_id and not completed:
                log(f"failed: deleting {snapshot_id}")
                try:
                    ec2.delete_snapshot(SnapshotId=snapshot_id)
                    log(f"{snapshot_id} deleted")
                except Exception as error:  # noqa: BLE001 - the cleanup reports whatever stops it
                    log(f"could not delete {snapshot_id} ({error}); delete it by hand: "
                        f"./deploy/run.sh aws ec2 delete-snapshot --snapshot-id {snapshot_id}")
    print(snapshot_id, flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        log("interrupted")
        sys.exit(130)
