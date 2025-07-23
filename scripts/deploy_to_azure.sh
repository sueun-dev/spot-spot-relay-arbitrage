#!/bin/bash

# Azure VM Deployment Script for Kimchi Premium Bot
# This script sets up the bot on Azure VM from scratch

echo "🚀 Starting Azure VM Deployment..."
echo "================================="

# 1. System Update
echo "📦 Updating system packages..."
sudo apt update && sudo apt upgrade -y

# 2. Install required packages
echo "🔧 Installing Python and dependencies..."
sudo apt install -y python3 python3-pip python3-venv git tmux htop

# 3. Clone repository
echo "📥 Cloning repository from GitHub..."
cd ~
if [ -d "kimp_arb_bot" ]; then
    echo "Repository already exists, updating..."
    cd kimp_arb_bot
    git pull origin main
else
    git clone https://github.com/sueun-dev/kimp_arb_bot.git
    cd kimp_arb_bot
fi

# 4. Set up Python virtual environment
echo "🐍 Setting up Python virtual environment..."
if [ ! -d "venv" ]; then
    python3 -m venv venv
fi
source venv/bin/activate

# 5. Install Python packages
echo "📚 Installing Python packages..."
pip install --upgrade pip
pip install -r pyproject.toml

# 6. Create .env file from example
echo "⚙️ Setting up environment configuration..."
if [ ! -f ".env" ]; then
    cp .env.example .env
    echo "⚠️  IMPORTANT: Edit .env file with your actual API keys!"
    echo "   Run: nano .env"
fi

# 7. Create logs directory
mkdir -p logs

# 8. Set up systemd service
echo "🔧 Setting up systemd service..."
sudo tee /etc/systemd/system/kimchi-bot.service > /dev/null <<EOF
[Unit]
Description=Kimchi Premium Trading Bot
After=network.target

[Service]
Type=simple
User=azureuser
WorkingDirectory=/home/azureuser/kimp_arb_bot
Environment="PATH=/home/azureuser/kimp_arb_bot/venv/bin"
ExecStart=/home/azureuser/kimp_arb_bot/venv/bin/python /home/azureuser/kimp_arb_bot/main.py
Restart=always
RestartSec=10
StandardOutput=append:/home/azureuser/kimp_arb_bot/logs/systemd.log
StandardError=append:/home/azureuser/kimp_arb_bot/logs/systemd.log

[Install]
WantedBy=multi-user.target
EOF

# 9. Enable but don't start the service yet
sudo systemctl daemon-reload
sudo systemctl enable kimchi-bot

# 10. Set permissions
echo "🔒 Setting file permissions..."
chmod 600 .env
chmod +x scripts/*.sh

# 11. Create helper script for starting the bot
cat > ~/start_bot.sh << 'EOL'
#!/bin/bash
cd ~/kimp_arb_bot
source venv/bin/activate
python main.py
EOL
chmod +x ~/start_bot.sh

echo "✅ Deployment complete!"
echo ""
echo "📋 Next steps:"
echo "1. Edit .env file with your API keys: nano ~/kimp_arb_bot/.env"
echo "2. Test the setup: cd ~/kimp_arb_bot && source venv/bin/activate && python tests/test_verify_exchanges.py"
echo "3. Start bot with tmux: tmux new -s bot && ~/start_bot.sh"
echo "4. Or start with systemd: sudo systemctl start kimchi-bot"
echo "5. Check logs: tail -f ~/kimp_arb_bot/logs/bot_*.log"
echo ""
echo "💡 Useful commands:"
echo "- Check bot status: sudo systemctl status kimchi-bot"
echo "- View logs: sudo journalctl -u kimchi-bot -f"
echo "- Attach to tmux: tmux attach -t bot"
echo "- Update from GitHub: cd ~/kimp_arb_bot && git pull"