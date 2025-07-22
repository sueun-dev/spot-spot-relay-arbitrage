# 🌐 Azure VM Connection Guide

## Your VM Details
- **Public IP**: 20.41.115.143
- **Private IP**: 10.0.0.4
- **Network Security**: SSH (port 22) is OPEN ✅
- **VM Name**: codebase817_z1

## 🔐 Connection Methods

### Method 1: Using Existing SSH Key
If you created the VM with your existing SSH key:
```bash
ssh -i ~/.ssh/id_ed25519 azureuser@20.41.115.143
```

### Method 2: Using Azure-Generated SSH Key
If Azure generated a key when creating the VM:
1. Download the private key from Azure Portal
2. Save it to your ~/.ssh/ directory
3. Set proper permissions:
```bash
chmod 600 ~/.ssh/your_azure_key.pem
```
4. Connect:
```bash
ssh -i ~/.ssh/your_azure_key.pem azureuser@20.41.115.143
```

### Method 3: Reset SSH Credentials in Azure
If you don't have the correct SSH key:

1. **Go to Azure Portal**
2. **Navigate to your VM** (codebase817_z1)
3. **Click "Reset password"** (under Support + troubleshooting)
4. **Choose "Reset SSH public key"**
5. **Enter**:
   - Username: azureuser (or your preferred username)
   - SSH public key: Copy contents of `~/.ssh/id_ed25519.pub`
6. **Click "Update"**

### Method 4: Enable Password Authentication (Not Recommended)
1. In Azure Portal → VM → Reset password
2. Choose "Reset password"
3. Set a password for azureuser
4. Connect: `ssh azureuser@20.41.115.143` (enter password when prompted)

## 🛠️ Quick Connection Script

```bash
# Save your VM connection as an alias
echo "alias azure-vm='ssh -i ~/.ssh/id_ed25519 azureuser@20.41.115.143'" >> ~/.zshrc
source ~/.zshrc

# Now you can connect with:
azure-vm
```

## 📋 After Connecting

Once connected, you can:
```bash
# Check system info
uname -a
lsb_release -a

# Update system
sudo apt update && sudo apt upgrade -y

# Install required software
sudo apt install python3 python3-pip git -y

# Clone your bot
git clone https://github.com/yourusername/kimp_arb_bot.git
cd kimp_arb_bot

# Set up environment
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

## 🔧 Troubleshooting

### "Permission denied (publickey)"
- You're using the wrong SSH key
- Solution: Reset SSH key in Azure Portal (Method 3)

### "Connection refused"
- SSH service might not be running
- Solution: Check VM status in Azure Portal

### "Host key verification failed"
- First time connecting
- Solution: Type "yes" when prompted

## 🚀 Run Bot on Azure VM

1. **Transfer .env file**:
```bash
# From your local machine
scp -i ~/.ssh/id_ed25519 .env azureuser@20.41.115.143:~/kimp_arb_bot/
```

2. **Start bot in background**:
```bash
# On Azure VM
cd ~/kimp_arb_bot
nohup python main.py > bot.log 2>&1 &
```

3. **Monitor logs**:
```bash
tail -f bot.log
```

## 📊 VM Management

### Check running processes
```bash
ps aux | grep python
```

### Stop the bot
```bash
pkill -f "python main.py"
```

### Auto-start on boot
```bash
# Create systemd service
sudo nano /etc/systemd/system/kimchi-bot.service
```

Add:
```ini
[Unit]
Description=Kimchi Premium Bot
After=network.target

[Service]
Type=simple
User=azureuser
WorkingDirectory=/home/azureuser/kimp_arb_bot
ExecStart=/home/azureuser/kimp_arb_bot/venv/bin/python main.py
Restart=always

[Install]
WantedBy=multi-user.target
```

Enable:
```bash
sudo systemctl enable kimchi-bot
sudo systemctl start kimchi-bot
```