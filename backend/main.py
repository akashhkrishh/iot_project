from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, field_validator, model_validator
from Crypto.Cipher import AES
from Crypto.Util.Padding import unpad, pad
import base64
import json
import sqlite3
import re
from typing import List, Optional
from datetime import datetime, date


app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# --- SQLite Setup ---
conn = sqlite3.connect('hospital.db', check_same_thread=False)
cursor = conn.cursor()

cursor.execute('''
    CREATE TABLE IF NOT EXISTS doctors (
        username TEXT PRIMARY KEY,
        password TEXT NOT NULL
    )
''')
cursor.execute('''
    CREATE TABLE IF NOT EXISTS patients (
        mobile TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        gender TEXT NOT NULL DEFAULT 'Other',
        dob TEXT,
        blood_group TEXT,
        address TEXT
    )
''')
cursor.execute('''
    CREATE TABLE IF NOT EXISTS medical_readings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        mobile TEXT NOT NULL,
        temp REAL,
        hr INTEGER,
        spo2 INTEGER,
        timestamp TEXT NOT NULL,
        FOREIGN KEY (mobile) REFERENCES patients (mobile)
    )
''')
# Migrate existing DB: add new columns if they don't exist yet
for col_def in [
    ("gender", "TEXT NOT NULL DEFAULT 'Other'"),
    ("dob", "TEXT"),
    ("blood_group", "TEXT"),
    ("address", "TEXT")
]:
    try:
        cursor.execute(f"ALTER TABLE patients ADD COLUMN {col_def[0]} {col_def[1]}")
    except sqlite3.OperationalError:
        pass  # column already exists
cursor.execute("INSERT OR IGNORE INTO doctors (username, password) VALUES ('dr_smith', 'password123')")
conn.commit()


# --- Security/Encryption Constants ---
KEY = b'my_secure_16_key'
IV = b'my_secure_16_iv_'

def decrypt_payload(base64_ciphertext: str) -> dict:
    try:
        ciphertext = base64.b64decode(base64_ciphertext)
        cipher = AES.new(KEY, AES.MODE_CBC, IV)
        decrypted_bytes = unpad(cipher.decrypt(ciphertext), AES.block_size)
        return json.loads(decrypted_bytes.decode('utf-8'))
    except Exception as e:
        raise ValueError("Decryption failed")

def encrypt_payload(json_data: dict) -> str:
    try:
        data_string = json.dumps(json_data, separators=(',', ':'))
        cipher = AES.new(KEY, AES.MODE_CBC, IV)
        padded_data = pad(data_string.encode('utf-8'), AES.block_size)
        encrypted_bytes = cipher.encrypt(padded_data)
        return base64.b64encode(encrypted_bytes).decode('utf-8')
    except Exception as e:
        raise ValueError("Encryption failed")


# --- WebSocket Managers ---
class BaseConnectionManager:
    def __init__(self):
        self.active_connections: List[WebSocket] = []

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)

    def disconnect(self, websocket: WebSocket):
        self.active_connections.remove(websocket)

class ESPManager(BaseConnectionManager):
    async def command_stream(self, action: str, mobile: str = ""):
        if not self.active_connections:
            raise HTTPException(status_code=503, detail="ESP Hardware Offline")
        cmd_dict = {"cmd": action, "mobile": mobile} if action == "start_stream" else {"cmd": action}
        print(f"Dispatching to ESP: {cmd_dict}")
        encrypted_cmd = encrypt_payload(cmd_dict)
        msg = {"data": encrypted_cmd}
        for connection in self.active_connections:
            await connection.send_text(json.dumps(msg, separators=(',', ':')))

class FrontendManager(BaseConnectionManager):
    async def proxy_live_data(self, unencrypted_json: dict):
        for connection in self.active_connections:
            await connection.send_text(json.dumps(unencrypted_json))

esp_manager = ESPManager()
frontend_manager = FrontendManager()


VALID_BLOOD_GROUPS = {"A+", "A-", "B+", "B-", "AB+", "AB-", "O+", "O-"}
VALID_GENDERS     = {"Male", "Female", "Other"}

# --- Routes ---
class LoginRequest(BaseModel):
    username: str
    password: str

    @field_validator("username", "password")
    @classmethod
    def not_empty(cls, v: str, info) -> str:
        v = v.strip()
        if not v:
            raise ValueError(f"{info.field_name} cannot be empty")
        if len(v) > 50:
            raise ValueError(f"{info.field_name} is too long (max 50 chars)")
        return v

@app.post("/api/login")
async def login(req: LoginRequest):
    cursor.execute("SELECT * FROM doctors WHERE username = ? AND password = ?", (req.username, req.password))
    if cursor.fetchone():
        return {"status": "success", "token": "mock_token_123"}
    raise HTTPException(status_code=401, detail="Invalid credentials")


class PatientPayload(BaseModel):
    name: str
    mobile: str
    gender: str
    dob: Optional[str] = None
    blood_group: Optional[str] = None
    address: Optional[str] = None

    @field_validator("name")
    @classmethod
    def validate_name(cls, v: str) -> str:
        v = v.strip()
        if not v:
            raise ValueError("Name cannot be empty")
        if len(v) < 2:
            raise ValueError("Name must be at least 2 characters")
        if len(v) > 80:
            raise ValueError("Name is too long (max 80 characters)")
        if not re.match(r"^[A-Za-z\s\.\-']+$", v):
            raise ValueError("Name can only contain letters, spaces, hyphens, dots and apostrophes")
        return v

    @field_validator("mobile")
    @classmethod
    def validate_mobile(cls, v: str) -> str:
        v = v.strip()
        if not re.match(r"^\d{10}$", v):
            raise ValueError("Mobile must be exactly 10 digits (numbers only)")
        return v

    @field_validator("gender")
    @classmethod
    def validate_gender(cls, v: str) -> str:
        v = v.strip()
        if v not in VALID_GENDERS:
            raise ValueError(f"Gender must be one of: {', '.join(VALID_GENDERS)}")
        return v

    @field_validator("dob")
    @classmethod
    def validate_dob(cls, v: Optional[str]) -> Optional[str]:
        if not v or not v.strip():
            return None
        v = v.strip()
        try:
            dob_date = datetime.strptime(v, "%Y-%m-%d").date()
        except ValueError:
            raise ValueError("Date of birth must be in YYYY-MM-DD format")
        today = date.today()
        if dob_date >= today:
            raise ValueError("Date of birth must be in the past")
        age_years = (today - dob_date).days / 365.25
        if age_years > 120:
            raise ValueError("Date of birth seems too far in the past (max age 120 years)")
        return v

    @field_validator("blood_group")
    @classmethod
    def validate_blood_group(cls, v: Optional[str]) -> Optional[str]:
        if not v or not v.strip():
            return None
        v = v.strip().upper()
        if v not in VALID_BLOOD_GROUPS:
            raise ValueError(f"Blood group must be one of: {', '.join(sorted(VALID_BLOOD_GROUPS))}")
        return v

    @field_validator("address")
    @classmethod
    def validate_address(cls, v: Optional[str]) -> Optional[str]:
        if not v or not v.strip():
            return None
        v = v.strip()
        if len(v) > 200:
            raise ValueError("Address is too long (max 200 characters)")
        return v

@app.post("/api/patients")
async def register_patient(req: PatientPayload):
    try:
        cursor.execute(
            "INSERT INTO patients (mobile, name, gender, dob, blood_group, address) VALUES (?, ?, ?, ?, ?, ?)",
            (req.mobile, req.name, req.gender, req.dob, req.blood_group, req.address)
        )
        conn.commit()
        return {"status": "success"}
    except sqlite3.IntegrityError:
        raise HTTPException(status_code=400, detail="A patient with this mobile number already exists")

@app.get("/api/patients")
async def get_patients():
    cursor.execute("SELECT mobile, name, gender, dob, blood_group, address FROM patients")
    patients = cursor.fetchall()
    patients_list = []
    for (mobile, name, gender, dob, blood_group, address) in patients:
        cursor.execute("SELECT temp, hr, spo2, timestamp FROM medical_readings WHERE mobile = ? ORDER BY id DESC LIMIT 1", (mobile,))
        latest = cursor.fetchone()
        reading = None
        if latest:
            reading = {"temp": latest[0], "hr": latest[1], "spo2": latest[2], "timestamp": latest[3]}
        patients_list.append({"mobile": mobile, "name": name, "gender": gender, "dob": dob, "blood_group": blood_group, "address": address, "latest_reading": reading})
    return patients_list

@app.get("/api/patients/{mobile}")
async def get_patient_detail(mobile: str):
    if not re.match(r"^\d{10}$", mobile):
        raise HTTPException(status_code=400, detail="Invalid mobile number format")
    cursor.execute("SELECT mobile, name, gender, dob, blood_group, address FROM patients WHERE mobile = ?", (mobile,))
    row = cursor.fetchone()
    if not row:
        raise HTTPException(status_code=404, detail="Patient not found")
    return {"mobile": row[0], "name": row[1], "gender": row[2], "dob": row[3], "blood_group": row[4], "address": row[5]}


class UpdatePatientPayload(BaseModel):
    name: str
    gender: str
    dob: Optional[str] = None
    blood_group: Optional[str] = None
    address: Optional[str] = None

    @field_validator("name")
    @classmethod
    def validate_name(cls, v: str) -> str:
        v = v.strip()
        if not v or len(v) < 2:
            raise ValueError("Name must be at least 2 characters")
        if len(v) > 80:
            raise ValueError("Name is too long (max 80 characters)")
        if not re.match(r"^[A-Za-z\s\.\-']+$", v):
            raise ValueError("Name can only contain letters, spaces, hyphens, dots and apostrophes")
        return v

    @field_validator("gender")
    @classmethod
    def validate_gender(cls, v: str) -> str:
        if v.strip() not in VALID_GENDERS:
            raise ValueError(f"Gender must be one of: {', '.join(VALID_GENDERS)}")
        return v.strip()

    @field_validator("dob")
    @classmethod
    def validate_dob(cls, v: Optional[str]) -> Optional[str]:
        if not v or not v.strip():
            return None
        try:
            dob_date = datetime.strptime(v.strip(), "%Y-%m-%d").date()
        except ValueError:
            raise ValueError("Date of birth must be in YYYY-MM-DD format")
        if dob_date >= date.today():
            raise ValueError("Date of birth must be in the past")
        if (date.today() - dob_date).days / 365.25 > 120:
            raise ValueError("Date of birth seems too far in the past")
        return v.strip()

    @field_validator("blood_group")
    @classmethod
    def validate_blood_group(cls, v: Optional[str]) -> Optional[str]:
        if not v or not v.strip():
            return None
        v = v.strip().upper()
        if v not in VALID_BLOOD_GROUPS:
            raise ValueError(f"Blood group must be one of: {', '.join(sorted(VALID_BLOOD_GROUPS))}")
        return v

    @field_validator("address")
    @classmethod
    def validate_address(cls, v: Optional[str]) -> Optional[str]:
        if not v or not v.strip():
            return None
        v = v.strip()
        if len(v) > 200:
            raise ValueError("Address too long (max 200 chars)")
        return v

@app.put("/api/patients/{mobile}")
async def update_patient(mobile: str, req: UpdatePatientPayload):
    if not re.match(r"^\d{10}$", mobile):
        raise HTTPException(status_code=400, detail="Invalid mobile number format")
    cursor.execute("SELECT mobile FROM patients WHERE mobile = ?", (mobile,))
    if not cursor.fetchone():
        raise HTTPException(status_code=404, detail="Patient not found")
    cursor.execute(
        "UPDATE patients SET name = ?, gender = ?, dob = ?, blood_group = ?, address = ? WHERE mobile = ?",
        (req.name, req.gender, req.dob, req.blood_group, req.address, mobile)
    )
    conn.commit()
    return {"status": "updated"}


@app.delete("/api/patients/{mobile}")
async def delete_patient(mobile: str):
    cursor.execute("DELETE FROM medical_readings WHERE mobile = ?", (mobile,))
    cursor.execute("DELETE FROM patients WHERE mobile = ?", (mobile,))
    conn.commit()
    if cursor.rowcount == 0:
        raise HTTPException(status_code=404, detail="Patient not found")
    return {"status": "deleted"}

@app.delete("/api/patients/{mobile}/readings")
async def delete_all_readings(mobile: str):
    cursor.execute("DELETE FROM medical_readings WHERE mobile = ?", (mobile,))
    conn.commit()
    return {"status": "deleted", "count": cursor.rowcount}


@app.get("/api/system/status")
async def system_status():
    return {"esp_online": len(esp_manager.active_connections) > 0}


@app.post("/api/patients/{mobile}/trigger_read")
async def trigger_patient_read(mobile: str):
    await esp_manager.command_stream("start_stream", mobile)
    return {"status": "success", "message": "ESP Streaming Initated"}

@app.post("/api/patients/{mobile}/stop_read")
async def stop_patient_read(mobile: str):
    await esp_manager.command_stream("stop_stream")
    return {"status": "success", "message": "ESP Streaming Terminated"}

class FinalizeReadingPayload(BaseModel):
    temp: float
    hr: int
    spo2: int

@app.post("/api/patients/{mobile}/finalize_reading")
async def finalize_reading(mobile: str, p: FinalizeReadingPayload):
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    cursor.execute(
        "INSERT INTO medical_readings (mobile, temp, hr, spo2, timestamp) VALUES (?, ?, ?, ?, ?)",
        (mobile, p.temp, p.hr, p.spo2, now)
    )
    conn.commit()
    return {"status": "success"}

@app.get("/api/patients/{mobile}/readings")
async def get_patient_readings(mobile: str):
    cursor.execute(
        "SELECT id, temp, hr, spo2, timestamp FROM medical_readings WHERE mobile = ? ORDER BY id DESC LIMIT 20",
        (mobile,)
    )
    rows = cursor.fetchall()
    return [{"id": r[0], "temp": r[1], "hr": r[2], "spo2": r[3], "timestamp": r[4]} for r in rows]

@app.delete("/api/patients/{mobile}/readings/{reading_id}")
async def delete_reading(mobile: str, reading_id: int):
    cursor.execute("DELETE FROM medical_readings WHERE id = ? AND mobile = ?", (reading_id, mobile))
    conn.commit()
    if cursor.rowcount == 0:
        raise HTTPException(status_code=404, detail="Reading not found")
    return {"status": "deleted"}


# --- WebSockets ---
@app.websocket("/ws/sensor")
async def esp_ws_endpoint(websocket: WebSocket):
    await esp_manager.connect(websocket)
    try:
        while True:
            incoming_text = await websocket.receive_text()
            print(f"RAW WS RCVD: {incoming_text}")
            try:
                payload_dict = json.loads(incoming_text)
                encrypted_str = payload_dict.get("data")
                if encrypted_str:
                    decrypted_json = decrypt_payload(encrypted_str)
                    print(f"Decrypted proxy relay: {decrypted_json}")
                    
                    # Instead of math, transparently route down to React's UI Graph!
                    await frontend_manager.proxy_live_data(decrypted_json)
            except Exception as parse_error:
                print(f"Failed to decrypt ESP stream: {parse_error}")
    except WebSocketDisconnect:
        esp_manager.disconnect(websocket)
        print("ESP hardware dropped.")

@app.websocket("/ws/frontend")
async def ui_ws_endpoint(websocket: WebSocket):
    await frontend_manager.connect(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        frontend_manager.disconnect(websocket)
