#!/usr/bin/env bash

export _OS_NAME=$1 
envsubst < GRUB_TEMPLATE
