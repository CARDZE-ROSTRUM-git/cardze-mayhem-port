# Minimal image that ships the prebuilt desktop binary.
#
# GitHub Packages can only host package formats (containers, npm, nuget, ...),
# not arbitrary files, so the ready-to-run executable is published as a
# container image. The image content is just the binary (plus a short note),
# so `docker create` + `docker export` (or any registry browser) hands back the
# exact file from the release.
FROM scratch
COPY mayhem-desktop /mayhem-desktop
COPY CONTAINER-README.txt /README.txt
LABEL org.opencontainers.image.title="cardze-mayhem-port" \
      org.opencontainers.image.description="Desktop port of the PortaPack Mayhem firmware (SDL2)." \
      org.opencontainers.image.source="https://github.com/CARDZE-ROSTRUM-git/cardze-mayhem-port" \
      org.opencontainers.image.licenses="GPL-2.0-or-later"
ENTRYPOINT ["/mayhem-desktop"]
