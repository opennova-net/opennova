# The hosted game servers' data snapshot helper (data_snapshot.py; DEPLOY.md
# "Hosted game servers"). The deploy toolbox builds it on the operator's own
# docker (`./deploy/run.sh hosted data-snapshot`) and streams the Joint
# Operations files into it; it is never published, and it holds no game byte:
# the data arrives on its standard input at run time and leaves with the
# container (--rm). e2fsprogs builds the ext4 image (mkfs.ext4 -d); boto3 drives
# the EBS direct APIs.

FROM python:3.12.7-slim-bookworm

RUN apt-get update \
    && apt-get install -y --no-install-recommends e2fsprogs \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir --require-hashes --only-binary=:all: --no-deps -r /tmp/requirements.txt \
    && rm /tmp/requirements.txt

COPY data_snapshot.py /usr/local/bin/data_snapshot.py

ENTRYPOINT ["python3", "/usr/local/bin/data_snapshot.py"]
