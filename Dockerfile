FROM alpine:3.20 AS build

RUN apk add --no-cache build-base
WORKDIR /app
COPY Makefile .
COPY src ./src
RUN make

FROM alpine:3.20

WORKDIR /app
COPY --from=build /app/build/rinha-api /app/rinha-api
COPY --from=build /app/build/rinha-lb /app/rinha-lb
RUN mkdir -p /app/resources
COPY resources/references.bin /app/resources/references.bin
COPY resources/references.idx /app/resources/references.idx

RUN if [ ! -f /app/resources/references.idx ]; then \
      echo "missing preprocessed references"; exit 1; \
    fi

ENV PORT=8080
ENV REFERENCES_PATH=/app/resources/references.idx

EXPOSE 8080
CMD ["/app/rinha-api"]
