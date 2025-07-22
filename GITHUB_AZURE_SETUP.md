# 🚀 GitHub & Azure VM Setup Guide

## ⚠️ 보안 체크리스트 (중요!)

### 1. GitHub에 올리기 전 확인사항
- ✅ `.env` 파일이 `.gitignore`에 포함되어 있는지 확인
- ✅ API 키가 코드에 하드코딩되어 있지 않은지 확인
- ✅ `.env.example`만 올라가고 실제 `.env`는 제외되는지 확인

## 📤 GitHub에 코드 올리기

### 1. 첫 커밋 및 푸시
```bash
# 1. Git 초기화 (이미 완료)
git init

# 2. 모든 파일 추가
git add .

# 3. 민감한 파일 제외 확인
git status
# .env 파일이 없어야 함!

# 4. 첫 커밋
git commit -m "Initial commit - Kimchi Premium Arbitrage Bot"

# 5. GitHub 연결
git remote add origin https://github.com/sueun-dev/kimp_arb_bot.git

# 6. 메인 브랜치로 변경
git branch -M main

# 7. GitHub에 푸시
git push -u origin main
```

## 🌐 Azure VM 설정

### 1. VM 연결
```bash
# SSH 키를 이용한 연결 (이미 작동 확인됨)
ssh -i ~/.ssh/codebase_key.pem azureuser@20.41.115.143
```

### 2. VM에서 봇 설치
```bash
# 1. 시스템 업데이트
sudo apt update && sudo apt upgrade -y

# 2. 필수 패키지 설치
sudo apt install python3 python3-pip python3-venv git -y

# 3. GitHub에서 코드 클론
cd ~
git clone https://github.com/sueun-dev/kimp_arb_bot.git
cd kimp_arb_bot

# 4. Python 가상환경 생성
python3 -m venv venv
source venv/bin/activate

# 5. 패키지 설치
pip install -r requirements.txt
```

### 3. 환경 변수 설정 (중요!)
```bash
# 1. .env 파일 생성
cp .env.example .env

# 2. .env 파일 편집
nano .env

# 3. 실제 API 키 입력 (로컬 .env에서 복사)
# - UPBIT_ACCESS_KEY=실제키
# - UPBIT_SECRET_KEY=실제키
# - 등등...

# 4. 파일 저장 (Ctrl+X, Y, Enter)
```

### 4. 봇 실행

#### 방법 1: tmux 사용 (추천)
```bash
# tmux 설치
sudo apt install tmux -y

# 새 세션 시작
tmux new -s kimchi-bot

# 봇 실행
source venv/bin/activate
python main.py

# tmux에서 나가기: Ctrl+B, D
# 다시 연결: tmux attach -t kimchi-bot
```

#### 방법 2: systemd 서비스로 실행
```bash
# 1. 서비스 파일 생성
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

```bash
# 2. 서비스 활성화 및 시작
sudo systemctl daemon-reload
sudo systemctl enable kimchi-bot
sudo systemctl start kimchi-bot

# 3. 상태 확인
sudo systemctl status kimchi-bot

# 4. 로그 보기
sudo journalctl -u kimchi-bot -f
```

## 🔐 보안 팁

### 1. VM 방화벽 설정
```bash
# SSH만 허용 (이미 설정됨)
sudo ufw allow 22/tcp
sudo ufw enable
```

### 2. .env 파일 보호
```bash
# 소유자만 읽기 가능하도록 설정
chmod 600 ~/kimp_arb_bot/.env
```

### 3. 정기적인 업데이트
```bash
# GitHub에서 최신 코드 가져오기
cd ~/kimp_arb_bot
git pull origin main

# 봇 재시작
sudo systemctl restart kimchi-bot
```

## 📊 모니터링

### 로그 확인
```bash
# systemd 로그
sudo journalctl -u kimchi-bot -f

# 또는 직접 로그 파일
tail -f ~/kimp_arb_bot/logs/bot_*.log
```

### 프로세스 확인
```bash
# 실행 중인지 확인
ps aux | grep python

# CPU/메모리 사용량
htop
```

## 🆘 문제 해결

### 봇이 시작되지 않을 때
1. 환경 변수 확인: `cat .env`
2. Python 경로 확인: `which python`
3. 에러 로그 확인: `sudo journalctl -u kimchi-bot -n 100`

### API 연결 실패
1. VM에서 인터넷 연결 확인: `ping google.com`
2. API 키 형식 확인 (공백, 따옴표 없이)
3. 거래소 API 상태 확인

## ✅ 완료!

이제 봇이 Azure VM에서 24/7 실행됩니다!