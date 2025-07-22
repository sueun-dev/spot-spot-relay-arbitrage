#!/bin/bash

# Azure VM Connection Helper
VM_IP="20.41.115.143"
SSH_KEY="~/.ssh/codebase_key.pem"
USERNAME="azureuser"

echo "🌐 Connecting to Azure VM (Ubuntu 24.04)"
echo "   IP: $VM_IP"
echo ""

# Connect
ssh -i $SSH_KEY $USERNAME@$VM_IP