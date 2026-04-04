import inspect
from sqlalchemy import text
from sqlalchemy.ext.asyncio import create_async_engine, async_sessionmaker, AsyncSession
from models import Base
from config import DATABASE_URL

engine = create_async_engine(DATABASE_URL, echo=False)
async_session = async_sessionmaker(engine, class_=AsyncSession, expire_on_commit=False)


def _get_model_columns(table_name: str):
    """Return {col_name: Column} for a given table from the ORM metadata."""
    table = Base.metadata.tables.get(table_name)
    if table is None:
        return {}
    return {c.name: c for c in table.columns}


def _col_type_sql(col) -> str:
    """Best-effort SQL type string for an ALTER TABLE ADD COLUMN."""
    from sqlalchemy.types import Integer, String, Text, DateTime, Enum
    t = col.type
    if isinstance(t, Integer):
        return "INTEGER"
    if isinstance(t, String):
        return f"VARCHAR({t.length})" if t.length else "VARCHAR(255)"
    if isinstance(t, Text):
        return "TEXT"
    if isinstance(t, DateTime):
        return "DATETIME"
    if isinstance(t, Enum):
        return "VARCHAR(50)"
    return "TEXT"


async def _migrate_table(conn, table_name: str):
    """Add any columns present in the ORM model but missing from the DB table."""
    result = await conn.execute(text(f"PRAGMA table_info({table_name})"))
    existing = {row[1] for row in result.fetchall()}
    model_cols = _get_model_columns(table_name)
    for col_name, col in model_cols.items():
        if col_name not in existing:
            sql_type = _col_type_sql(col)
            stmt = f"ALTER TABLE {table_name} ADD COLUMN {col_name} {sql_type}"
            print(f"[DB MIGRATE] {stmt}")
            await conn.execute(text(stmt))


async def init_db():
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)
        for table_name in Base.metadata.tables:
            await _migrate_table(conn, table_name)
    print("[DB] Initialized and migrated")


async def get_db() -> AsyncSession:
    async with async_session() as session:
        yield session
