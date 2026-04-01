#!/usr/bin/env python3
"""
Cloud backend for M5StickC Audio Platform.

Handles device pairing, audio streaming via WebSocket, real-time
transcription (ElevenLabs Scribe), LLM processing, and serves the web portal.
"""

import asyncio
import hashlib
import json
import os
import secrets
import struct
import wave
import io
from datetime import datetime, timedelta
from typing import Optional

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Depends, HTTPException, UploadFile, File, Form, Request, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse, StreamingResponse
from pydantic import BaseModel, EmailStr
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession
from packaging.version import Version

from database import init_db, get_db, async_session
from models import User, Device, Session, SessionStatus, Transcription, Note, NoteType, FirmwareVersion, CanvasArt
from auth import hash_password, verify_password, create_access_token, get_current_user
from adpcm import AdpcmDecoder
from config import ELEVENLABS_API_KEY, OPENAI_API_KEY, PAIRING_CODE_EXPIRY_SECONDS

app = FastAPI(title="XIAO Audio Platform")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ── Connected WebSockets ──
user_connections: dict[int, list[WebSocket]] = {}    # user_id -> list of browser sockets
device_connections: dict[int, WebSocket] = {}         # device_id -> device socket


@app.on_event("startup")
async def startup():
    await init_db()


# ═══════════════════════════════════════════════════════════════
# REST API: Auth
# ═══════════════════════════════════════════════════════════════

class RegisterRequest(BaseModel):
    email: str
    password: str

class LoginRequest(BaseModel):
    email: str
    password: str

class TokenResponse(BaseModel):
    access_token: str
    token_type: str = "bearer"


@app.post("/api/auth/register", response_model=TokenResponse)
async def register(req: RegisterRequest, db: AsyncSession = Depends(get_db)):
    existing = await db.execute(select(User).where(User.email == req.email))
    if existing.scalar_one_or_none():
        raise HTTPException(status_code=400, detail="Email already registered")

    user = User(email=req.email, password_hash=hash_password(req.password))
    db.add(user)
    await db.commit()
    await db.refresh(user)

    return TokenResponse(access_token=create_access_token(user.id))


@app.post("/api/auth/login", response_model=TokenResponse)
async def login(req: LoginRequest, db: AsyncSession = Depends(get_db)):
    result = await db.execute(select(User).where(User.email == req.email))
    user = result.scalar_one_or_none()
    if not user or not verify_password(req.password, user.password_hash):
        raise HTTPException(status_code=401, detail="Invalid credentials")

    return TokenResponse(access_token=create_access_token(user.id))


@app.get("/api/auth/me")
async def get_me(user: User = Depends(get_current_user)):
    return {"id": user.id, "email": user.email}


# ═══════════════════════════════════════════════════════════════
# REST API: Device Registration & Pairing
# ═══════════════════════════════════════════════════════════════

class DeviceRegisterRequest(BaseModel):
    mac: str
    code: str

class DevicePairRequest(BaseModel):
    code: str


@app.post("/api/devices/register", status_code=201)
async def register_device(req: DeviceRegisterRequest, db: AsyncSession = Depends(get_db)):
    """Called by the M5StickC to register itself with a pairing code."""
    result = await db.execute(select(Device).where(Device.mac_address == req.mac))
    device = result.scalar_one_or_none()

    expires = datetime.utcnow() + timedelta(seconds=PAIRING_CODE_EXPIRY_SECONDS)

    if device:
        device.pairing_code = req.code
        device.pairing_code_expires_at = expires
    else:
        device = Device(
            mac_address=req.mac,
            pairing_code=req.code,
            pairing_code_expires_at=expires,
        )
        db.add(device)

    await db.commit()
    return {"status": "registered", "expires_in": PAIRING_CODE_EXPIRY_SECONDS}


@app.get("/api/devices/status")
async def device_status(mac: str, db: AsyncSession = Depends(get_db)):
    """Polled by M5StickC to check if pairing completed. Returns token when paired."""
    result = await db.execute(select(Device).where(Device.mac_address == mac))
    device = result.scalar_one_or_none()

    if not device:
        raise HTTPException(status_code=404, detail="Device not found")

    if device.api_token and device.user_id:
        return {"paired": True, "token": device.api_token}

    return {"paired": False}


@app.post("/api/devices/pair")
async def pair_device(
    req: DevicePairRequest,
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """Called by web portal when user enters the pairing code."""
    result = await db.execute(
        select(Device).where(
            Device.pairing_code == req.code,
            Device.user_id.is_(None),
        )
    )
    device = result.scalar_one_or_none()

    if not device:
        raise HTTPException(status_code=404, detail="Invalid or expired pairing code")

    if device.pairing_code_expires_at and device.pairing_code_expires_at < datetime.utcnow():
        raise HTTPException(status_code=410, detail="Pairing code expired")

    device.user_id = user.id
    device.api_token = secrets.token_hex(32)
    device.pairing_code = None
    device.pairing_code_expires_at = None
    device.paired_at = datetime.utcnow()
    await db.commit()

    return {"status": "paired", "device_name": device.name, "device_id": device.id}


class DeviceAutoRegisterRequest(BaseModel):
    mac: str
    email: str
    password: str
    action: str = "login"


@app.post("/api/devices/register-with-user", status_code=200)
async def register_device_with_user(req: DeviceAutoRegisterRequest, db: AsyncSession = Depends(get_db)):
    """Called by device during setup to pair itself with a user account in one step.
    action='login' validates existing credentials; action='signup' creates a new account."""
    if req.action == "signup":
        existing = await db.execute(select(User).where(User.email == req.email))
        if existing.scalar_one_or_none():
            raise HTTPException(status_code=400, detail="Email already registered")
        user = User(email=req.email, password_hash=hash_password(req.password))
        db.add(user)
        await db.commit()
        await db.refresh(user)
    else:
        result = await db.execute(select(User).where(User.email == req.email))
        user = result.scalar_one_or_none()
        if not user or not verify_password(req.password, user.password_hash):
            raise HTTPException(status_code=401, detail="Invalid credentials")

    result = await db.execute(select(Device).where(Device.mac_address == req.mac))
    device = result.scalar_one_or_none()

    if device and device.user_id and device.api_token:
        return {"paired": True, "token": device.api_token}

    if not device:
        device = Device(mac_address=req.mac)
        db.add(device)

    device.user_id = user.id
    device.api_token = secrets.token_hex(32)
    device.pairing_code = None
    device.pairing_code_expires_at = None
    device.paired_at = datetime.utcnow()
    await db.commit()

    return {"paired": True, "token": device.api_token}


@app.get("/api/devices")
async def list_devices(user: User = Depends(get_current_user), db: AsyncSession = Depends(get_db)):
    result = await db.execute(select(Device).where(Device.user_id == user.id))
    devices = result.scalars().all()
    return [
        {
            "id": d.id,
            "name": d.name,
            "mac_address": d.mac_address,
            "paired_at": str(d.paired_at),
            "online": d.id in device_connections,
        }
        for d in devices
    ]


class WifiUpdateRequest(BaseModel):
    ssid: str
    password: str


@app.post("/api/devices/{device_id}/wifi")
async def update_device_wifi(
    device_id: int,
    req: WifiUpdateRequest,
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """Push new WiFi credentials to a connected device."""
    result = await db.execute(
        select(Device).where(Device.id == device_id, Device.user_id == user.id)
    )
    device = result.scalar_one_or_none()
    if not device:
        raise HTTPException(status_code=404, detail="Device not found")

    ws = device_connections.get(device_id)
    if not ws:
        raise HTTPException(status_code=409, detail="Device is offline")

    try:
        await ws.send_json({
            "type": "wifi_update",
            "ssid": req.ssid,
            "password": req.password,
        })
    except Exception:
        device_connections.pop(device_id, None)
        raise HTTPException(status_code=409, detail="Device connection lost")

    return {"status": "sent", "device_id": device_id}


# ═══════════════════════════════════════════════════════════════
# REST API: OTA Firmware Updates
# ═══════════════════════════════════════════════════════════════

FIRMWARE_DIR = os.path.join(os.path.dirname(__file__), "firmware")
os.makedirs(FIRMWARE_DIR, exist_ok=True)


async def authenticate_device(token: str, mac: str, db: AsyncSession) -> Device:
    result = await db.execute(
        select(Device).where(Device.api_token == token, Device.mac_address == mac)
    )
    device = result.scalar_one_or_none()
    if not device or not device.user_id:
        raise HTTPException(status_code=401, detail="Invalid device credentials")
    return device


@app.get("/api/ota/check")
async def check_ota(
    mac: str,
    version: str,
    token: str,
    db: AsyncSession = Depends(get_db),
):
    """Device polls this to check if a newer firmware is available."""
    device = await authenticate_device(token, mac, db)

    device.firmware_version = version
    await db.commit()

    result = await db.execute(
        select(FirmwareVersion)
        .where(FirmwareVersion.is_active == 1)
        .order_by(FirmwareVersion.created_at.desc())
    )
    firmwares = result.scalars().all()

    try:
        current = Version(version)
    except Exception:
        return {"update": False}

    for fw in firmwares:
        try:
            if Version(fw.version) > current:
                return {
                    "update": True,
                    "version": fw.version,
                    "firmware_id": fw.id,
                    "sha256": fw.sha256,
                    "size": fw.file_size,
                }
        except Exception:
            continue

    return {"update": False}


@app.get("/api/ota/firmware/{firmware_id}")
async def download_firmware(
    firmware_id: int,
    token: str,
    mac: str,
    db: AsyncSession = Depends(get_db),
):
    """Device downloads the firmware binary for OTA flashing."""
    await authenticate_device(token, mac, db)

    result = await db.execute(
        select(FirmwareVersion).where(FirmwareVersion.id == firmware_id)
    )
    fw = result.scalar_one_or_none()
    if not fw:
        raise HTTPException(status_code=404, detail="Firmware not found")

    filepath = os.path.join(FIRMWARE_DIR, fw.filename)
    if not os.path.isfile(filepath):
        raise HTTPException(status_code=404, detail="Firmware file missing from disk")

    def iter_file():
        with open(filepath, "rb") as f:
            while chunk := f.read(4096):
                yield chunk

    return StreamingResponse(
        iter_file(),
        media_type="application/octet-stream",
        headers={
            "Content-Length": str(fw.file_size),
            "X-Firmware-SHA256": fw.sha256,
        },
    )


@app.post("/api/ota/upload", status_code=201)
async def upload_firmware(
    file: UploadFile = File(...),
    version: str = Form(...),
    release_notes: str = Form(""),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """Upload a new firmware binary. Auth: user JWT."""
    existing = await db.execute(
        select(FirmwareVersion).where(FirmwareVersion.version == version)
    )
    if existing.scalar_one_or_none():
        raise HTTPException(status_code=409, detail=f"Version {version} already exists")

    contents = await file.read()
    sha256 = hashlib.sha256(contents).hexdigest()
    filename = f"{version}.bin"
    filepath = os.path.join(FIRMWARE_DIR, filename)

    with open(filepath, "wb") as f:
        f.write(contents)

    fw = FirmwareVersion(
        version=version,
        filename=filename,
        sha256=sha256,
        file_size=len(contents),
        release_notes=release_notes or None,
    )
    db.add(fw)
    await db.commit()
    await db.refresh(fw)

    print(f"Firmware uploaded: v{version}, {len(contents)} bytes, sha256={sha256[:16]}...")

    return {
        "id": fw.id,
        "version": fw.version,
        "sha256": fw.sha256,
        "size": fw.file_size,
    }


# ═══════════════════════════════════════════════════════════════
# REST API: Sessions & Notes
# ═══════════════════════════════════════════════════════════════

@app.get("/api/sessions")
async def list_sessions(user: User = Depends(get_current_user), db: AsyncSession = Depends(get_db)):
    result = await db.execute(
        select(Session)
        .where(Session.user_id == user.id)
        .order_by(Session.started_at.desc())
    )
    sessions = result.scalars().all()
    return [
        {
            "id": s.id,
            "started_at": str(s.started_at),
            "ended_at": str(s.ended_at) if s.ended_at else None,
            "status": s.status.value,
        }
        for s in sessions
    ]


@app.get("/api/sessions/{session_id}")
async def get_session(session_id: int, user: User = Depends(get_current_user), db: AsyncSession = Depends(get_db)):
    result = await db.execute(
        select(Session).where(Session.id == session_id, Session.user_id == user.id)
    )
    session = result.scalar_one_or_none()
    if not session:
        raise HTTPException(status_code=404, detail="Session not found")

    t_result = await db.execute(
        select(Transcription)
        .where(Transcription.session_id == session_id, Transcription.is_final == 1)
        .order_by(Transcription.timestamp_start)
    )
    transcriptions = t_result.scalars().all()

    n_result = await db.execute(
        select(Note).where(Note.session_id == session_id).order_by(Note.created_at)
    )
    notes = n_result.scalars().all()

    return {
        "id": session.id,
        "started_at": str(session.started_at),
        "ended_at": str(session.ended_at) if session.ended_at else None,
        "status": session.status.value,
        "transcript": " ".join(t.text for t in transcriptions),
        "notes": [
            {"id": n.id, "type": n.type.value, "content": n.content}
            for n in notes
        ],
    }


# ═══════════════════════════════════════════════════════════════
# REST API: Canvas Art (Pixel Doodle Studio)
# ═══════════════════════════════════════════════════════════════

CANVAS_WIDTH = 48
CANVAS_HEIGHT = 27
CANVAS_PIXELS = CANVAS_WIDTH * CANVAS_HEIGHT  # 1296


class CanvasSaveRequest(BaseModel):
    name: str
    pixel_data: str


class CanvasSendRequest(BaseModel):
    pixel_data: str


@app.post("/api/canvas", status_code=201)
async def save_canvas(
    req: CanvasSaveRequest,
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    count_result = await db.execute(
        select(CanvasArt).where(CanvasArt.user_id == user.id)
    )
    if len(count_result.scalars().all()) >= 10:
        raise HTTPException(status_code=400, detail="Maximum 10 saved canvases reached")

    art = CanvasArt(user_id=user.id, name=req.name, pixel_data=req.pixel_data)
    db.add(art)
    await db.commit()
    await db.refresh(art)
    return {"id": art.id, "name": art.name, "created_at": str(art.created_at)}


@app.get("/api/canvas")
async def list_canvas(user: User = Depends(get_current_user), db: AsyncSession = Depends(get_db)):
    result = await db.execute(
        select(CanvasArt)
        .where(CanvasArt.user_id == user.id)
        .order_by(CanvasArt.created_at.desc())
    )
    arts = result.scalars().all()
    return [
        {"id": a.id, "name": a.name, "pixel_data": a.pixel_data, "created_at": str(a.created_at)}
        for a in arts
    ]


@app.delete("/api/canvas/{canvas_id}")
async def delete_canvas(
    canvas_id: int,
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    result = await db.execute(
        select(CanvasArt).where(CanvasArt.id == canvas_id, CanvasArt.user_id == user.id)
    )
    art = result.scalar_one_or_none()
    if not art:
        raise HTTPException(status_code=404, detail="Canvas not found")
    await db.delete(art)
    await db.commit()
    return {"status": "deleted"}


@app.post("/api/canvas/send/{device_id}")
async def send_canvas_to_device(
    device_id: int,
    req: CanvasSendRequest,
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """Push pixel art to a connected device."""
    result = await db.execute(
        select(Device).where(Device.id == device_id, Device.user_id == user.id)
    )
    device = result.scalar_one_or_none()
    if not device:
        raise HTTPException(status_code=404, detail="Device not found")

    ws = device_connections.get(device_id)
    if not ws:
        raise HTTPException(status_code=409, detail="Device is offline")

    try:
        await ws.send_json({
            "type": "display_update",
            "width": CANVAS_WIDTH,
            "height": CANVAS_HEIGHT,
            "pixels": req.pixel_data,
        })
    except Exception:
        device_connections.pop(device_id, None)
        raise HTTPException(status_code=409, detail="Device connection lost")

    return {"status": "sent", "device_id": device_id}


# ═══════════════════════════════════════════════════════════════
# REST API: Offline WAV Upload (from device SD card)
# ═══════════════════════════════════════════════════════════════

@app.post("/api/upload", status_code=201)
async def upload_wav(request: Request, db: AsyncSession = Depends(get_db)):
    """Accept a raw WAV file upload from a device that recorded offline."""
    token = request.headers.get("X-Device-Token", "")
    mac = request.headers.get("X-Device-MAC", "")

    if not token or not mac:
        raise HTTPException(status_code=401, detail="Missing device credentials")

    result = await db.execute(
        select(Device).where(Device.api_token == token, Device.mac_address == mac)
    )
    device = result.scalar_one_or_none()
    if not device or not device.user_id:
        raise HTTPException(status_code=401, detail="Invalid device credentials")

    body = await request.body()
    if len(body) < 44:
        raise HTTPException(status_code=400, detail="File too small to be a valid WAV")

    try:
        wav_file = wave.open(io.BytesIO(body), 'rb')
        sample_rate = wav_file.getframerate()
        pcm_audio = wav_file.readframes(wav_file.getnframes())
        wav_file.close()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid WAV file")

    session = Session(
        device_id=device.id,
        user_id=device.user_id,
        status=SessionStatus.PROCESSING,
        ended_at=datetime.utcnow(),
    )
    db.add(session)
    await db.commit()
    await db.refresh(session)

    sid = session.id
    uid = device.user_id

    async def process_uploaded():
        transcript = await run_transcription(sid, uid, bytearray(pcm_audio), sample_rate)
        if transcript:
            await run_llm_processing(sid, uid, transcript)
        else:
            async with async_session() as db2:
                s = await db2.execute(select(Session).where(Session.id == sid))
                sess = s.scalar_one_or_none()
                if sess:
                    sess.status = SessionStatus.DONE
                    await db2.commit()

    asyncio.create_task(process_uploaded())

    return {"status": "accepted", "session_id": session.id}


# ═══════════════════════════════════════════════════════════════
# WebSocket: Device Audio Stream
# ═══════════════════════════════════════════════════════════════

async def notify_user(user_id: int, message: dict):
    """Push a JSON message to all connected browser sessions for a user."""
    if user_id in user_connections:
        dead = []
        for ws in user_connections[user_id]:
            try:
                await ws.send_json(message)
            except Exception:
                dead.append(ws)
        for ws in dead:
            user_connections[user_id].remove(ws)


async def run_transcription(session_id: int, user_id: int, pcm_audio: bytearray, sample_rate: int):
    """
    Send audio to ElevenLabs Scribe v2 for transcription. If ELEVENLABS_API_KEY
    is not set, falls back to a placeholder that just logs audio stats.
    """
    if not ELEVENLABS_API_KEY:
        num_samples = len(pcm_audio) // 2
        duration = num_samples / sample_rate
        print(f"  [transcribe placeholder] session={session_id}, {duration:.1f}s audio, no ElevenLabs key set")
        return

    try:
        import httpx

        wav_buffer = io.BytesIO()
        with wave.open(wav_buffer, 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(sample_rate)
            wf.writeframes(pcm_audio)
        wav_bytes = wav_buffer.getvalue()

        async with httpx.AsyncClient(timeout=120) as client:
            resp = await client.post(
                "https://api.elevenlabs.io/v1/speech-to-text",
                headers={"xi-api-key": ELEVENLABS_API_KEY},
                files={"file": ("audio.wav", wav_bytes, "audio/wav")},
                data={"model_id": "scribe_v2"},
            )
            resp.raise_for_status()
            result = resp.json()

        transcript = result.get("text", "")

        if not transcript:
            print(f"  [ElevenLabs] session={session_id}: empty transcript returned")
            return None

        async with async_session() as db:
            t = Transcription(
                session_id=session_id,
                text=transcript,
                is_final=1,
                timestamp_start=datetime.utcnow(),
            )
            db.add(t)
            await db.commit()

        await notify_user(user_id, {
            "type": "transcript_final",
            "session_id": session_id,
            "text": transcript,
        })

        print(f"  [ElevenLabs] session={session_id}: {transcript[:100]}...")
        return transcript

    except Exception as e:
        print(f"  [ElevenLabs error] {e}")
        return None


async def run_llm_processing(session_id: int, user_id: int, transcript: str):
    """Send full transcript to LLM for note/task extraction."""
    if not OPENAI_API_KEY or not transcript:
        print(f"  [LLM placeholder] session={session_id}, no OpenAI key or empty transcript")
        return

    try:
        from openai import OpenAI

        client = OpenAI(api_key=OPENAI_API_KEY)

        prompt = f"""Analyze this meeting/conversation transcript and extract structured information.
Return a JSON object with these keys:
- "summary": A 2-3 sentence summary of the conversation
- "action_items": An array of specific action items or tasks mentioned
- "decisions": An array of decisions that were made
- "notes": An array of other important points or observations

Transcript:
{transcript}"""

        response = await asyncio.to_thread(
            lambda: client.chat.completions.create(
                model="gpt-4o-mini",
                messages=[{"role": "user", "content": prompt}],
                response_format={"type": "json_object"},
            )
        )

        result = json.loads(response.choices[0].message.content)

        async with async_session() as db:
            if result.get("summary"):
                db.add(Note(
                    session_id=session_id, user_id=user_id,
                    type=NoteType.SUMMARY, content=result["summary"],
                ))
            for item in result.get("action_items", []):
                db.add(Note(
                    session_id=session_id, user_id=user_id,
                    type=NoteType.ACTION_ITEM, content=item,
                ))
            for item in result.get("decisions", []):
                db.add(Note(
                    session_id=session_id, user_id=user_id,
                    type=NoteType.DECISION, content=item,
                ))
            for item in result.get("notes", []):
                db.add(Note(
                    session_id=session_id, user_id=user_id,
                    type=NoteType.NOTE, content=item,
                ))

            session_result = await db.execute(select(Session).where(Session.id == session_id))
            session = session_result.scalar_one_or_none()
            if session:
                session.status = SessionStatus.DONE
            await db.commit()

        await notify_user(user_id, {
            "type": "session_processed",
            "session_id": session_id,
            "summary": result.get("summary", ""),
            "action_items": result.get("action_items", []),
            "decisions": result.get("decisions", []),
            "notes": result.get("notes", []),
        })

        print(f"  [LLM] session={session_id}: processed, {len(result.get('action_items', []))} action items")

    except Exception as e:
        print(f"  [LLM error] {e}")


@app.websocket("/ws/device")
async def ws_device(websocket: WebSocket):
    """WebSocket endpoint for M5StickC device audio streaming."""
    await websocket.accept()

    token = websocket.query_params.get("token", "")
    mac = websocket.query_params.get("mac", "")

    async with async_session() as db:
        result = await db.execute(
            select(Device).where(Device.api_token == token, Device.mac_address == mac)
        )
        device = result.scalar_one_or_none()

    if not device or not device.user_id:
        await websocket.send_json({"type": "auth_error", "message": "Invalid token or unpaired device"})
        await websocket.close()
        return

    user_id = device.user_id
    device_id = device.id
    device_connections[device_id] = websocket
    await websocket.send_json({"type": "auth_ok"})

    fw_version = websocket.query_params.get("fw", "")
    if fw_version:
        async with async_session() as db_fw:
            result_fw = await db_fw.execute(select(Device).where(Device.id == device_id))
            dev = result_fw.scalar_one_or_none()
            if dev:
                dev.firmware_version = fw_version
                await db_fw.commit()

    print(f"Device connected: MAC={mac}, device_id={device_id}, user_id={user_id}, fw={fw_version}")

    decoder = AdpcmDecoder()
    audio_pcm = bytearray()
    sample_rate = 16000
    streaming = False
    chunk_count = 0
    current_session_id: Optional[int] = None

    try:
        while True:
            data = await websocket.receive_bytes()

            # Stop sentinel: 0xFFFFFFFF
            if len(data) == 4 and data == b'\xff\xff\xff\xff':
                if streaming:
                    duration = len(audio_pcm) / 2 / sample_rate
                    print(f"\nStream ended: {len(audio_pcm)} bytes ({duration:.1f}s), {chunk_count} chunks")
                    streaming = False

                    await notify_user(user_id, {
                        "type": "stream_end",
                        "session_id": current_session_id,
                        "duration": round(duration, 1),
                        "chunks": chunk_count,
                    })

                    # Process in background: transcribe then LLM
                    pcm_copy = bytes(audio_pcm)
                    sid = current_session_id
                    uid = user_id

                    async def process_session():
                        async with async_session() as db:
                            s = await db.execute(select(Session).where(Session.id == sid))
                            session = s.scalar_one_or_none()
                            if session:
                                session.ended_at = datetime.utcnow()
                                session.status = SessionStatus.PROCESSING
                                await db.commit()

                        transcript = await run_transcription(sid, uid, bytearray(pcm_copy), sample_rate)
                        if transcript:
                            await run_llm_processing(sid, uid, transcript)
                        else:
                            async with async_session() as db:
                                s = await db.execute(select(Session).where(Session.id == sid))
                                session = s.scalar_one_or_none()
                                if session:
                                    session.status = SessionStatus.DONE
                                    await db.commit()

                    asyncio.create_task(process_session())
                    audio_pcm = bytearray()
                continue

            # Start marker: 8 bytes [sample_rate:u32, 0x00000001:u32]
            if len(data) == 8:
                sr, marker = struct.unpack('<II', data)
                if marker == 1:
                    sample_rate = sr
                    decoder.reset()
                    audio_pcm = bytearray()
                    chunk_count = 0
                    streaming = True

                    async with async_session() as db:
                        session = Session(
                            device_id=device_id,
                            user_id=user_id,
                            status=SessionStatus.STREAMING,
                        )
                        db.add(session)
                        await db.commit()
                        await db.refresh(session)
                        current_session_id = session.id

                    await notify_user(user_id, {
                        "type": "stream_start",
                        "session_id": current_session_id,
                        "sample_rate": sample_rate,
                    })

                    print(f"Stream started: {sample_rate} Hz, session_id={current_session_id}")
                    continue

            if not streaming:
                continue

            # Audio data: decode ADPCM -> PCM
            pcm_chunk = decoder.decode(bytes(data))
            audio_pcm.extend(pcm_chunk)
            chunk_count += 1

            # Push audio level info to user periodically (every ~1s = ~31 chunks)
            if chunk_count % 31 == 0:
                total_sec = len(audio_pcm) / 2 / sample_rate
                num_samples = len(pcm_chunk) // 2
                pcm_values = struct.unpack(f"<{num_samples}h", pcm_chunk)
                rms = int((sum(s * s for s in pcm_values) / num_samples) ** 0.5) if num_samples else 0
                await notify_user(user_id, {
                    "type": "stream_progress",
                    "session_id": current_session_id,
                    "duration": round(total_sec, 1),
                    "chunks": chunk_count,
                    "rms": rms,
                })

    except WebSocketDisconnect:
        device_connections.pop(device_id, None)
        print(f"Device disconnected: MAC={mac}")
        if streaming and current_session_id and len(audio_pcm) > 0:
            duration = len(audio_pcm) / 2 / sample_rate
            print(f"  Disconnect mid-stream: {len(audio_pcm)} bytes ({duration:.1f}s), {chunk_count} chunks -- processing audio")

            await notify_user(user_id, {
                "type": "stream_end",
                "session_id": current_session_id,
                "duration": round(duration, 1),
                "chunks": chunk_count,
                "disconnected": True,
            })

            pcm_copy = bytes(audio_pcm)
            sid = current_session_id
            uid = user_id
            sr = sample_rate

            async def process_disconnected_session():
                async with async_session() as db:
                    s = await db.execute(select(Session).where(Session.id == sid))
                    session = s.scalar_one_or_none()
                    if session:
                        session.ended_at = datetime.utcnow()
                        session.status = SessionStatus.PROCESSING
                        await db.commit()

                transcript = await run_transcription(sid, uid, bytearray(pcm_copy), sr)
                if transcript:
                    await run_llm_processing(sid, uid, transcript)
                else:
                    async with async_session() as db:
                        s = await db.execute(select(Session).where(Session.id == sid))
                        session = s.scalar_one_or_none()
                        if session:
                            session.status = SessionStatus.DONE
                            await db.commit()

            asyncio.create_task(process_disconnected_session())
        elif streaming and current_session_id:
            print(f"  Disconnect mid-stream but no audio accumulated")
            async with async_session() as db:
                s = await db.execute(select(Session).where(Session.id == current_session_id))
                session = s.scalar_one_or_none()
                if session:
                    session.ended_at = datetime.utcnow()
                    session.status = SessionStatus.DONE
                    await db.commit()
    except Exception as e:
        device_connections.pop(device_id, None)
        print(f"Device WS error: {e}")
        if streaming and current_session_id:
            async with async_session() as db:
                s = await db.execute(select(Session).where(Session.id == current_session_id))
                session = s.scalar_one_or_none()
                if session:
                    session.ended_at = datetime.utcnow()
                    session.status = SessionStatus.DONE
                    await db.commit()


# ═══════════════════════════════════════════════════════════════
# WebSocket: User (Browser) Real-time Updates
# ═══════════════════════════════════════════════════════════════

@app.websocket("/ws/user")
async def ws_user(websocket: WebSocket):
    """WebSocket for web portal to receive live transcription and session updates."""
    await websocket.accept()

    token = websocket.query_params.get("token", "")
    if not token:
        await websocket.send_json({"type": "error", "message": "Missing token"})
        await websocket.close()
        return

    from jose import jwt, JWTError
    from config import JWT_SECRET, JWT_ALGORITHM

    try:
        payload = jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGORITHM])
        user_id = int(payload.get("sub"))
    except (JWTError, ValueError, TypeError):
        await websocket.send_json({"type": "error", "message": "Invalid token"})
        await websocket.close()
        return

    if user_id not in user_connections:
        user_connections[user_id] = []
    user_connections[user_id].append(websocket)

    await websocket.send_json({"type": "connected", "user_id": user_id})
    print(f"User WebSocket connected: user_id={user_id}")

    try:
        while True:
            data = await websocket.receive_text()
            msg = json.loads(data)
            if msg.get("type") == "ping":
                await websocket.send_json({"type": "pong"})
            elif msg.get("type") == "display_update":
                device_id = msg.get("device_id")
                dev_ws = device_connections.get(device_id)
                if dev_ws:
                    async with async_session() as db:
                        result = await db.execute(
                            select(Device).where(Device.id == device_id, Device.user_id == user_id)
                        )
                        if result.scalar_one_or_none():
                            try:
                                await dev_ws.send_json({
                                    "type": "display_update",
                                    "width": msg.get("width", CANVAS_WIDTH),
                                    "height": msg.get("height", CANVAS_HEIGHT),
                                    "pixels": msg.get("pixels", ""),
                                })
                                await websocket.send_json({"type": "display_sent"})
                            except Exception:
                                device_connections.pop(device_id, None)
                                await websocket.send_json({"type": "display_error", "message": "Device connection lost"})
                        else:
                            await websocket.send_json({"type": "display_error", "message": "Device not found"})
                else:
                    await websocket.send_json({"type": "display_error", "message": "Device is offline"})
    except WebSocketDisconnect:
        user_connections[user_id].remove(websocket)
        if not user_connections[user_id]:
            del user_connections[user_id]
        print(f"User WebSocket disconnected: user_id={user_id}")


import os
portal_dir = os.path.join(os.path.dirname(__file__), "portal")
if os.path.isdir(portal_dir):
    app.mount("/portal", StaticFiles(directory=portal_dir, html=True), name="portal")

    @app.get("/")
    async def root():
        return FileResponse(os.path.join(portal_dir, "index.html"))
else:
    @app.get("/")
    async def root():
        return {"status": "XIAO Audio Platform running"}
