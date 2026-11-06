from pathlib import Path
from engine.model_reasoning import reasoning_levels

def profile_configuration(providers, selected_provider, selected_model, max_tokens,
                          permission_mode='auto', permission_plugin=None, *,
                          proxy_url='', proxy_token='', own_config=False, session_root=None):
    """Compose the official profile; only the ephemeral local proxy token leaves Umeko."""
    if permission_mode not in ('auto', 'ask'):
        raise ValueError('DeepSeek Harness 不支持当前权限模式，请选择自动或询问。')
    plugin = Path(permission_plugin or Path(__file__).resolve().parents[1] / 'runtime/dsh-permissions.mjs')
    patch, environment = [], {}
    if not own_config:
        if not proxy_url or not proxy_token:
            raise RuntimeError('Umeko 内部 API 代理不可用，请重新启动 Umeko 后重试。')
        configured = {}
        for provider in providers:
            models = []
            for model in provider.get('models', []):
                levels = reasoning_levels(model)
                models.append({'id': model['alias'], 'name': model['alias'],
                               'contextWindow': model.get('contextWindow', 128000), 'maxTokens': max_tokens,
                               'reasoningEfforts': {'off': None, **{level: level for level in levels}} if levels else False})
            configured[provider['id']] = {
                'api': 'openai-completions', 'baseURL': proxy_url, 'apiKeyEnv': 'UMEKO_DSH_PROXY_TOKEN',
                'models': models, 'defaultMaxTokens': max_tokens,
                'compat': {'supportsDeveloperRole': False, 'thinkingFormat': 'openai'},
            }
        provider = next((p for p in providers if p['id'] == selected_provider), {})
        if not any(model['alias'] == selected_model for model in provider.get('models', [])):
            raise ValueError('请选择一个已在 Umeko 配置的可用模型。')
        selection = {'provider': selected_provider, 'model': selected_model}
        patch.extend([{'id': 'llm-pi-ai', 'config': {'providers': configured}},
                      {'id': 'acp', 'config': selection}, {'id': 'agent-default-model', 'config': selection}])
        environment['UMEKO_DSH_PROXY_TOKEN'] = proxy_token
    patch.extend([{'id': 'sandbox-policy', 'config': {'mode': 'workspace-write', 'additionalWritableRoots': []}},
             {'id': 'approval', 'config': {'policy': 'ask'}},
             {'id': 'permission', 'config': {'defaultPreset': 'workspace-write', 'presets': {
                 'read-only': {'sandbox': 'read-only', 'approval': 'ask'},
                 'workspace-write': {'sandbox': 'workspace-write', 'approval': 'ask'},
                 'danger-full-access': {'sandbox': 'danger-full-access', 'approval': 'never'}}}},
             {'insert': [{'id': 'umeko-tool-permissions', 'name': plugin.resolve().as_uri(),
                          'inject': ['tools', 'approval', 'sandboxPolicy', 'permissionPresets'],
                          'config': {'mode': permission_mode}}]},
             {'id': 'session-telemetry-otel', 'disabled': True}])
    if session_root is not None:
        patch.append({'id': 'session-persistence-jsonl', 'config': {'root': str(session_root)}})
    if not own_config:
        patch.extend({'id': name, 'disabled': True} for name in ('llm-deepseek', 'llm-deepseek-account', 'deepseek-account'))
    return patch, environment
