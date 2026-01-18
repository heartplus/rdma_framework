# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an RDMA (Remote Direct Memory Access) framework project. RDMA enables direct memory access from one computer to another without involving the operating system, CPU, or cache of either system.

## Development Status

This repository is currently empty and in initial setup phase. This file should be updated once the project structure is established with:
- build with cmake
- use .clang-format for c++ code style
- make sure compile pass

## Common RDMA Development Considerations

When developing RDMA applications, keep in mind:
- Memory registration and deregistration patterns
- Queue Pair (QP) state transitions and management
- Completion Queue (CQ) polling strategies
- Work Request (WR) posting and completion handling
- RDMA verbs API usage patterns
- InfiniBand vs RoCE (RDMA over Converged Ethernet) considerations
- use send and write for differenct case
- use polling instead of interrupt
- need to conside intergrate with spdk later
- can intergrate with brpc with mgmt path
