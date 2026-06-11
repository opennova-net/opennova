# Backup sidecar: nightly SQLite .backup -> S3, plus on-demand backup/list/
# restore driven by the deploy toolbox. AWS credentials come from the EC2
# instance's IAM role over IMDSv2 (no keys in the container).
FROM alpine:3.20

RUN apk add --no-cache aws-cli sqlite tini

COPY backup.sh list.sh restore.sh /backup/
COPY crontab /etc/crontabs/root
RUN chmod +x /backup/backup.sh /backup/list.sh /backup/restore.sh

# tini reaps crond's children cleanly. As a long-running sidecar this runs
# crond; the toolbox's `backup now`/`list` instead `compose run` the scripts
# directly, overriding this command.
ENTRYPOINT ["/sbin/tini", "--"]
CMD ["crond", "-f", "-l", "8"]
