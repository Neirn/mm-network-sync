FROM rust:1.85.0-slim as builder

# Set working directory
WORKDIR /usr/src/app

# Copy the necessary directories from the build context
COPY network-sync-rs/ network-sync-rs/
COPY network-sync-server/ network-sync-server/
COPY deps/ deps/

# Change to the server directory
WORKDIR /usr/src/app/network-sync-server

# Install dependencies and build the application
RUN apt-get update && apt-get install -y pkg-config libssl-dev && rm -rf /var/lib/apt/lists/*
RUN cargo build --release

# Create a smaller production image
FROM debian:bookworm-slim

# Install necessary runtime libraries
RUN apt-get update && apt-get install -y libssl-dev && rm -rf /var/lib/apt/lists/*

WORKDIR /app
# Copy the compiled binary from the builder stage
COPY --from=builder /usr/src/app/network-sync-server/target/release/network-sync-server .

# Expose the websocket port
EXPOSE 8080

# Command to run the server
CMD ["./network-sync-server", "--port", "8080"]
