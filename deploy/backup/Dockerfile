# Backup sidecar: a nightly pg_dump (custom format) of the NovaWorld service
# database to S3, plus the on-demand backup/list/restore the deploy toolbox
# drives. The copies are independent of the database instance (RDS keeps its
# own snapshots and point-in-time recovery beside them). AWS credentials come
# from the EC2 instance's IAM role over IMDSv2 (no keys in the container); the
# database's from DATABASE_URL and DATABASE_PASSWORD, as the server's.
#
# The PostgreSQL 17 client tools match the server's major version, so a dump
# taken here restores into the same server or a newer one.
FROM postgres:17-alpine

# The RDS CA bundle, pinned by its SHA-256 as the server image pins it
# (apps/novaworld_server/Dockerfile, which says how to bump it), at the same
# path, so the server's DATABASE_URL (sslmode=verify-full&sslrootcert=
# /app/rds-global-bundle.pem) works here unchanged.
ARG RDS_CA_BUNDLE_SHA256=90a6f1a7978a1b1b096dca6c57511ca48d3b8cf052f8e704baf358f34ea2f21b

RUN apk add --no-cache aws-cli bash curl tini \
    && mkdir -p /app \
    && curl -fsSL https://truststore.pki.rds.amazonaws.com/global/global-bundle.pem \
        -o /app/rds-global-bundle.pem \
    && echo "${RDS_CA_BUNDLE_SHA256}  /app/rds-global-bundle.pem" | sha256sum -c - \
    && apk del curl

COPY backup.sh list.sh restore.sh /backup/
COPY crontab /etc/crontabs/root
RUN chmod +x /backup/backup.sh /backup/list.sh /backup/restore.sh

# tini reaps crond's children cleanly. As a long-running sidecar this runs
# crond; the toolbox's `backup now`/`list`/`restore` instead `compose run` the
# scripts directly, overriding this command. The image's own entrypoint (the
# postgres server's) is replaced: this container never runs a server.
ENTRYPOINT ["/sbin/tini", "--"]
CMD ["crond", "-f", "-l", "8"]
