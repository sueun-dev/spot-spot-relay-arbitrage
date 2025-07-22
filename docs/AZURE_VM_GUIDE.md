# 🌐 Azure VM 연결 및 관리 가이드

## 📋 VM 정보
- **Public IP**: 20.41.115.143
- **Username**: azureuser
- **OS**: Ubuntu 24.04.2 LTS
- **SSH Key**: ~/.ssh/codebase_key.pem

## 🔗 VM 연결 방법

### 방법 1: 스크립트 사용 (추천)
```bash
./azure_vm_connect.sh
```

### 방법 2: Alias 사용
```bash
azure
```
*Note: 처음 사용시 `source ~/.zshrc` 실행 필요*

### 방법 3: 직접 SSH 연결
```bash
ssh -i ~/.ssh/codebase_key.pem azureuser@20.41.115.143
```

## 🚀 봇 설치 및 실행

### 1. VM에 필요한 패키지 설치
```bash
# VM에 연결한 후 실행
sudo apt update
sudo apt install python3 python3-pip python3-venv git tmux -y
```

### 2. 봇 코드 복사 (로컬 → VM)
```bash
# 로컬 터미널에서 실행
# 전체 폴더 복사
scp -i ~/.ssh/codebase_key.pem -r ~/Documents/kimp_arb_bot azureuser@20.41.115.143:~/

# .env 파일만 업데이트
scp -i ~/.ssh/codebase_key.pem ~/Documents/kimp_arb_bot/.env azureuser@20.41.115.143:~/kimp_arb_bot/
```

### 3. VM에서 봇 설정
```bash
# VM에 연결한 상태에서
cd ~/kimp_arb_bot

# 가상환경 생성 및 활성화
python3 -m venv venv
source venv/bin/activate

# 패키지 설치
pip install -r requirements.txt
```

### 4. 봇 실행 (백그라운드)

#### 옵션 1: tmux 사용 (추천)
```bash
# 새 tmux 세션 시작
tmux new -s kimchi-bot

# 봇 실행
cd ~/kimp_arb_bot
source venv/bin/activate
python main.py

# tmux 세션에서 나가기 (봇은 계속 실행됨)
# Ctrl+B, 그 다음 D 키 누르기

# 다시 연결하기
tmux attach -t kimchi-bot
```

#### 옵션 2: nohup 사용
```bash
cd ~/kimp_arb_bot
source venv/bin/activate
nohup python main.py > bot.log 2>&1 &

# 로그 확인
tail -f bot.log
```

#### 옵션 3: systemd 서비스 (자동 시작)
```bash
# 서비스 파일 생성
sudo nano /etc/systemd/system/kimchi-bot.service
```

다음 내용 입력:
```ini
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

[Install]
WantedBy=multi-user.target
```

서비스 활성화:
```bash
sudo systemctl daemon-reload
sudo systemctl enable kimchi-bot
sudo systemctl start kimchi-bot

# 상태 확인
sudo systemctl status kimchi-bot
```

## 🛑 봇 종료 방법

### tmux 사용시
```bash
# tmux 세션 연결
tmux attach -t kimchi-bot

# Ctrl+C로 봇 종료
# tmux 세션 종료: exit
```

### nohup 사용시
```bash
# 프로세스 찾기
ps aux | grep "python main.py"

# PID 확인 후 종료 (예: PID가 12345인 경우)
kill 12345

# 또는 한번에 종료
pkill -f "python main.py"
```

### systemd 서비스 사용시
```bash
# 봇 정지
sudo systemctl stop kimchi-bot

# 봇 재시작
sudo systemctl restart kimchi-bot

# 서비스 비활성화
sudo systemctl disable kimchi-bot
```

## 📊 모니터링

### 실시간 로그 확인
```bash
# nohup 사용시
tail -f ~/kimp_arb_bot/bot.log

# systemd 사용시
sudo journalctl -u kimchi-bot -f
```

### 리소스 사용량 확인
```bash
# CPU/메모리 사용량
htop

# 디스크 사용량
df -h

# 네트워크 상태
netstat -tuln
```

## 🔧 유용한 명령어

### 파일 동기화 (로컬 → VM)
```bash
# 변경된 파일만 동기화
rsync -avz -e "ssh -i ~/.ssh/codebase_key.pem" \
  ~/Documents/kimp_arb_bot/ \
  azureuser@20.41.115.143:~/kimp_arb_bot/
```

### VM에서 로그 파일 가져오기
```bash
# 로컬로 로그 복사
scp -i ~/.ssh/codebase_key.pem \
  azureuser@20.41.115.143:~/kimp_arb_bot/bot.log \
  ~/Downloads/
```

### SSH 연결 종료
```bash
# VM 터미널에서
exit

# 또는 Ctrl+D
```

## 🚨 문제 해결

### SSH 연결 안 될 때
```bash
# 권한 확인
ls -la ~/.ssh/codebase_key.pem
# 결과가 -rw------- 이어야 함

# 권한 수정
chmod 600 ~/.ssh/codebase_key.pem
```

### 봇이 실행 안 될 때
```bash
# 가상환경 활성화 확인
which python
# /home/azureuser/kimp_arb_bot/venv/bin/python 이어야 함

# 에러 로그 확인
tail -100 bot.log

# .env 파일 확인
ls -la ~/kimp_arb_bot/.env
```

### VM 재부팅 후
```bash
# systemd 서비스를 사용하지 않는 경우
# 수동으로 봇 재시작 필요
./azure_vm_connect.sh
tmux new -s kimchi-bot
cd ~/kimp_arb_bot && source venv/bin/activate && python main.py
```

## 💡 팁

1. **tmux 단축키**:
   - `Ctrl+B, D`: 세션에서 나가기 (detach)
   - `Ctrl+B, [`: 스크롤 모드 (q로 나가기)
   - `Ctrl+B, C`: 새 창 만들기
   - `Ctrl+B, N`: 다음 창으로

2. **자동 업데이트 스크립트**:
```bash
# update_bot.sh 만들기
cat > ~/update_bot.sh << 'EOF'
#!/bin/bash
cd ~/kimp_arb_bot
git pull
source venv/bin/activate
pip install -r requirements.txt
sudo systemctl restart kimchi-bot
EOF

chmod +x ~/update_bot.sh
```

3. **백업**:
```bash
# 중요 파일 백업
tar -czf kimchi_bot_backup_$(date +%Y%m%d).tar.gz \
  ~/kimp_arb_bot/.env \
  ~/kimp_arb_bot/logs/
```