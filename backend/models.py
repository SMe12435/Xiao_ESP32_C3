from datetime import datetime
from sqlalchemy import Column, Integer, String, DateTime, ForeignKey, Text, Enum as SAEnum
from sqlalchemy.orm import relationship, DeclarativeBase
import enum


class Base(DeclarativeBase):
    pass


class User(Base):
    __tablename__ = "users"

    id = Column(Integer, primary_key=True, autoincrement=True)
    email = Column(String(255), unique=True, nullable=False, index=True)
    password_hash = Column(String(255), nullable=False)
    created_at = Column(DateTime, default=datetime.utcnow)

    devices = relationship("Device", back_populates="user")
    sessions = relationship("Session", back_populates="user")


class Device(Base):
    __tablename__ = "devices"

    id = Column(Integer, primary_key=True, autoincrement=True)
    mac_address = Column(String(20), unique=True, nullable=False, index=True)
    api_token = Column(String(64), unique=True, nullable=True, index=True)
    user_id = Column(Integer, ForeignKey("users.id"), nullable=True)
    pairing_code = Column(String(6), nullable=True, index=True)
    pairing_code_expires_at = Column(DateTime, nullable=True)
    paired_at = Column(DateTime, nullable=True)
    name = Column(String(100), default="XIAO-Audio")
    firmware_version = Column(String(20), nullable=True)
    last_ota_at = Column(DateTime, nullable=True)

    user = relationship("User", back_populates="devices")
    sessions = relationship("Session", back_populates="device")


class SessionStatus(str, enum.Enum):
    STREAMING = "streaming"
    PROCESSING = "processing"
    DONE = "done"


class Session(Base):
    __tablename__ = "sessions"

    id = Column(Integer, primary_key=True, autoincrement=True)
    device_id = Column(Integer, ForeignKey("devices.id"), nullable=False)
    user_id = Column(Integer, ForeignKey("users.id"), nullable=False)
    started_at = Column(DateTime, default=datetime.utcnow)
    ended_at = Column(DateTime, nullable=True)
    status = Column(SAEnum(SessionStatus), default=SessionStatus.STREAMING)
    audio_file = Column(String(255), nullable=True)

    device = relationship("Device", back_populates="sessions")
    user = relationship("User", back_populates="sessions")
    transcriptions = relationship("Transcription", back_populates="session", order_by="Transcription.timestamp_start")
    notes = relationship("Note", back_populates="session")


class Transcription(Base):
    __tablename__ = "transcriptions"

    id = Column(Integer, primary_key=True, autoincrement=True)
    session_id = Column(Integer, ForeignKey("sessions.id"), nullable=False)
    text = Column(Text, nullable=False)
    is_final = Column(Integer, default=0)
    timestamp_start = Column(DateTime, nullable=True)
    timestamp_end = Column(DateTime, nullable=True)

    session = relationship("Session", back_populates="transcriptions")


class NoteType(str, enum.Enum):
    SUMMARY = "summary"
    ACTION_ITEM = "action_item"
    DECISION = "decision"
    NOTE = "note"


class Note(Base):
    __tablename__ = "notes"

    id = Column(Integer, primary_key=True, autoincrement=True)
    session_id = Column(Integer, ForeignKey("sessions.id"), nullable=False)
    user_id = Column(Integer, ForeignKey("users.id"), nullable=False)
    type = Column(SAEnum(NoteType), nullable=False)
    content = Column(Text, nullable=False)
    created_at = Column(DateTime, default=datetime.utcnow)

    session = relationship("Session", back_populates="notes")


class FirmwareVersion(Base):
    __tablename__ = "firmware_versions"

    id = Column(Integer, primary_key=True, autoincrement=True)
    version = Column(String(20), unique=True, nullable=False)
    filename = Column(String(255), nullable=False)
    sha256 = Column(String(64), nullable=False)
    file_size = Column(Integer, nullable=False)
    release_notes = Column(Text, nullable=True)
    is_active = Column(Integer, default=1)
    created_at = Column(DateTime, default=datetime.utcnow)


class CanvasArt(Base):
    __tablename__ = "canvas_art"

    id = Column(Integer, primary_key=True, autoincrement=True)
    user_id = Column(Integer, ForeignKey("users.id"), nullable=False)
    name = Column(String(100), nullable=False)
    pixel_data = Column(Text, nullable=False)
    created_at = Column(DateTime, default=datetime.utcnow)

    user = relationship("User")
