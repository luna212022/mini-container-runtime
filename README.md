# Mini Container Runtime (OS Project)

## Problem Statement
This project implements a mini container runtime inspired by OS concepts like process isolation and namespaces.

## Concepts Used
- PID Namespace
- Mount Namespace
- Network Namespace
- UTS Namespace
- User Namespace

## How it Works
The program creates an isolated environment (container) where processes run independently from the host system.

## How to Run

1. Build:
go build -o mini-runc .

2. Setup rootfs:
mkdir rootfs
tar -xpf alpine-minirootfs-*.tar.gz -C rootfs

3. Run:
./mini-runc run --rootfs=rootfs /bin/sh

## Output
- Isolated shell environment
- Separate hostname
- Independent process tree

## Files
- main.go → entry point
- run.go → namespace setup
- container.go → container execution

## Team Members
Vennela Shakthi VP
Yuvraj Pateria