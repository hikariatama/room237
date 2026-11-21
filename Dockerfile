# Existing Qt core unit suite; build inputs are fetched before the offline test run.
FROM debian:trixie-slim@sha256:a72b4433de1cf7d6bae0c4cee74d0547f58601a643b3375f070c51600d63b6b2 AS test
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build python3 ca-certificates libarchive-tools \
    ffmpeg qt6-base-dev libtss2-dev libudev-dev pkg-config perl && rm -rf /var/lib/apt/lists/*
ENV LANG=C.UTF-8
WORKDIR /src
COPY ci /src/ci
COPY qt/core qt/core
COPY qt/VERSION qt/VERSION
COPY qt/packaging/ci qt/packaging/ci
RUN cmake -S qt/core -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DROOM237_CORE_TESTS=ON -DCMAKE_PROJECT_INCLUDE=/src/ci/core-test-compat.cmake \
    && cmake --build /build --parallel 2
# Keep the report even when assertions fail: avx-ci reads the JUnit failures.
RUN --network=none mkdir /report && \
    (/build/room237_core_tests -o /report/junit.xml,junitxml -o -,txt || test -s /report/junit.xml)

FROM scratch AS test-report
COPY --from=test /report /

FROM nginx:1.31.9-alpine@sha256:fb4658584bd9a916ffb6f6e365f97c25cfdb2923801f551ea08356e695f14ff7
COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY landing/ /usr/share/nginx/html/
EXPOSE 80
HEALTHCHECK --interval=15s --timeout=3s --start-period=5s --retries=3 CMD wget -q --spider http://127.0.0.1/ || exit 1
