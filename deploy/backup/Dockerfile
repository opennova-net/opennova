# The backup task: a pg_dump (custom format) of the NovaWorld service database
# to S3, run once per task: nightly by EventBridge Scheduler and on demand by
# the deploy toolbox (backup/list/restore and the db commands, each a command
# of its own over the same task definition: infra/aws/backup.tf). The copies
# are independent of the database instance (RDS keeps its own snapshots and
# point-in-time recovery beside them). AWS credentials come from the task's
# role (no keys in the container); the database's from DATABASE_URL and
# DATABASE_PASSWORD, as the programs'.
#
# The PostgreSQL 17 client tools match the server's major version, so a dump
# taken here restores into the same server or a newer one.
#
# Published beside the programs' images (.github/workflows/novaworld-images.yml)
# and pinned by digest at each deploy. Its base is pinned by digest too, so
# every build is on the same postgres and the same Alpine branch. The tag names
# what the digest is; a bump moves both (docker buildx imagetools inspect
# postgres:<version>-alpine<branch>, its Digest line). The packages below come
# from that branch's repository at build time (its security fixes alone).
FROM postgres:17.11-alpine3.24@sha256:b0f9560a2de083e2cc7382e75f808c7381a32852a7ec49117deedb300e552b24

# The RDS CA bundle, pinned by its SHA-256 as the server image pins it
# (apps/novaworld_common/Dockerfile, which says how to bump it), at the same
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
RUN chmod +x /backup/backup.sh /backup/list.sh /backup/restore.sh

# One run, then the container ends: by default the dump (the schedule's run);
# the toolbox's one-off tasks override the command with the script they run.
# Nothing here runs cron: the schedule is EventBridge Scheduler's. tini as
# PID 1 forwards a stop's signal to the script. The image's own entrypoint (the
# postgres server's) is replaced: this container never runs a server.
ENTRYPOINT ["/sbin/tini", "--"]
CMD ["/backup/backup.sh"]
