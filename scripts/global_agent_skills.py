"""Inert global-agent Skill worker; never starts a model."""
import json
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "skill-runtime"))
sys.path.insert(0, str(ROOT / "provider-runtime"))
from umeko_skills.global_agent_skills_worker import execute


if __name__ == '__main__':
    try:
        result = execute(json.load(sys.stdin))
    except Exception as error:
        result = {'ok': False, 'error': str(error)[:1200]}
    print(json.dumps(result, ensure_ascii=False))
