#!/bin/bash

# Secure .env transfer script
# This script safely transfers your .env file to Azure VM

echo "🔐 Secure .env Transfer to Azure VM"
echo "=================================="

# Check if .env exists
if [ ! -f ".env" ]; then
    echo "❌ Error: .env file not found!"
    echo "Make sure you're in the project directory and .env exists."
    exit 1
fi

# VM details
VM_USER="azureuser"
VM_IP="20.41.115.143"
SSH_KEY="~/.ssh/codebase_key.pem"

echo "📤 Transferring .env file to Azure VM..."
scp -i $SSH_KEY .env $VM_USER@$VM_IP:~/kimp_arb_bot/.env

if [ $? -eq 0 ]; then
    echo "✅ .env file transferred successfully!"
    
    # Set proper permissions on the VM
    echo "🔒 Setting secure permissions..."
    ssh -i $SSH_KEY $VM_USER@$VM_IP "chmod 600 ~/kimp_arb_bot/.env"
    
    echo ""
    echo "✅ Transfer complete!"
    echo ""
    echo "📋 Next steps:"
    echo "1. Connect to VM: ssh -i $SSH_KEY $VM_USER@$VM_IP"
    echo "2. Test the bot: cd ~/kimp_arb_bot && source venv/bin/activate && python tests/test_verify_exchanges.py"
    echo "3. Start trading: sudo systemctl start kimchi-bot"
    echo "4. Check logs: sudo journalctl -u kimchi-bot -f"
else
    echo "❌ Error: Failed to transfer .env file!"
    echo "Please check your connection and try again."
    exit 1
fi