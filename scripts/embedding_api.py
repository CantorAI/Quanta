"""
File: embedding_api.py

A unified API for multiple embedding backends with on-demand model loading,
pooling strategies, and device (CPU/GPU) selection.
Supported backends:
 - SentenceTransformers (st)
 - HuggingFace Transformers (hf)
 - OpenAI Embeddings (openai)
 - TensorFlow Hub USE (use)
"""
from typing import List, Optional, Dict, Any
import numpy as np


def _mean_pooling(hidden_states, attention_mask):
    import torch
    mask = attention_mask.unsqueeze(-1).expand(hidden_states.size()).float()
    summed = torch.sum(hidden_states * mask, dim=1)
    counts = torch.clamp(mask.sum(dim=1), min=1e-9)
    return summed / counts


def _max_pooling(hidden_states, attention_mask):
    import torch
    mask = attention_mask.unsqueeze(-1).expand(hidden_states.size()).float()
    hidden_states = hidden_states.clone()
    hidden_states[mask == 0] = -1e9
    return torch.max(hidden_states, dim=1).values


class BaseEmbedder:
    def embed_text(self, text: str, **kwargs) -> np.ndarray:
        raise NotImplementedError

    def embed_chunks(self, chunks: List[str], **kwargs) -> np.ndarray:
        raise NotImplementedError


class STEmbedder(BaseEmbedder):
    """SentenceTransformers embedder (uses built-in pooling)."""
    def __init__(self, model_name: str, device: str = 'cpu', **kwargs):
        from sentence_transformers import SentenceTransformer
        self.model = SentenceTransformer(model_name, device=device, **kwargs)

    def embed_text(self, text: str, **kwargs) -> np.ndarray:
        return self.model.encode([text], convert_to_numpy=True, **kwargs)[0]

    def embed_chunks(self, chunks: List[str], **kwargs) -> np.ndarray:
        return self.model.encode(chunks, convert_to_numpy=True, **kwargs)


class HFEmbedder(BaseEmbedder):
    """
    HuggingFace Transformer embedder with configurable pooling:
    - pooling: 'mean', 'cls', 'max'
    """
    def __init__(self,
                 model_name: str,
                 device: str = 'cpu',
                 pooling: str = 'mean',
                 **kwargs):
        from transformers import AutoTokenizer, AutoModel
        import torch
        self.tokenizer = AutoTokenizer.from_pretrained(model_name)
        self.model = AutoModel.from_pretrained(model_name, **kwargs).to(device)
        self.device = device
        self.pooling = pooling.lower()

    def embed_text(self, text: str, **kwargs) -> np.ndarray:
        import torch
        inputs = self.tokenizer(
            text,
            return_tensors='pt',
            truncation=True,
            padding=True
        ).to(self.device)
        outputs = self.model(**inputs)
        hidden = outputs.last_hidden_state
        mask = inputs.attention_mask

        if self.pooling == 'cls':
            pooled = hidden[:, 0]
        elif self.pooling == 'max':
            pooled = _max_pooling(hidden, mask)
        else:
            pooled = _mean_pooling(hidden, mask)

        return pooled.cpu().detach().numpy()[0]

    def embed_chunks(self, chunks: List[str], **kwargs) -> np.ndarray:
        embs = [self.embed_text(c, **kwargs) for c in chunks]
        return np.vstack(embs)


class OpenAIEmbedder(BaseEmbedder):
    """OpenAI Embeddings API embedder."""
    def __init__(self,
                 model_name: str = 'text-embedding-ada-002',
                 api_key: Optional[str] = None):
        import openai, os
        openai.api_key = api_key or os.getenv('OPENAI_API_KEY')
        self.model_name = model_name

    def embed_text(self, text: str, **kwargs) -> np.ndarray:
        import openai
        resp = openai.Embedding.create(input=text, model=self.model_name)
        return np.array(resp['data'][0]['embedding'])

    def embed_chunks(self, chunks: List[str], **kwargs) -> np.ndarray:
        import openai
        resp = openai.Embedding.create(input=chunks, model=self.model_name)
        return np.array([d['embedding'] for d in resp['data']])


class USEEmbedder(BaseEmbedder):
    """TensorFlow Hub Universal Sentence Encoder embedder."""
    def __init__(self,
                 model_url: str = 'https://tfhub.dev/google/universal-sentence-encoder/4'):
        import tensorflow_hub as hub
        self.embed = hub.load(model_url)

    def embed_text(self, text: str, **kwargs) -> np.ndarray:
        return np.array(self.embed([text]))[0]

    def embed_chunks(self, chunks: List[str], **kwargs) -> np.ndarray:
        return np.array(self.embed(chunks))


class EmbeddingAPI:
    """
    Manage multiple embedder backends with on-demand loading.

    Defaults registry (not pre-loaded):
      st:
        'mpnet': 'all-mpnet-base-v2'
        'mini': 'all-MiniLM-L6-v2'
      hf:
        'e5': 'intfloat/e5-large-v2' (mean pooling)
        'instructor': 'hkunlp/instructor-xl' (cls pooling)
      openai:
        'ada': 'text-embedding-ada-002'
      use:
        'use': default TF Hub USE
    """
    def __init__(self, default_device: str = None):
        if default_device is None:
                try:
                    import torch
                    default_device = 'cuda' if torch.cuda.is_available() else 'cpu'
                except ImportError:
                    default_device = 'cpu'
        self._registry: Dict[str, Dict[str, Any]] = {}
        self._models: Dict[str, BaseEmbedder] = {}
        self._active: Optional[str] = None
        self.default_device = default_device

        # Setup default registry without loading
        self._registry.update({
            'mpnet':    {'backend': 'st',     'model': 'all-mpnet-base-v2', 'device': default_device},
            'mini':     {'backend': 'st',     'model': 'all-MiniLM-L6-v2', 'device': default_device},
            'e5':       {'backend': 'hf',     'model': 'intfloat/e5-large-v2', 'device': default_device, 'pooling': 'mean'},
            'instructor':{'backend': 'hf',    'model': 'hkunlp/instructor-xl', 'device': default_device, 'pooling': 'cls'},
            'ada':      {'backend': 'openai','model': 'text-embedding-ada-002'},
            'use':      {'backend': 'use'}
        })

    def add_model(self,
                  name: str,
                  backend: str,
                  model_name_or_path: Optional[str] = None,
                  **kwargs) -> None:
        """
        Register or override a model config without immediate load.
        """
        self._registry[name] = {'backend': backend, 'model': model_name_or_path, **kwargs}
        if name in self._models:
            del self._models[name]
        if self._active is None:
            self._active = name

    def _load_model(self, name: str) -> BaseEmbedder:
        config = self._registry.get(name)
        if not config:
            raise ValueError(f"Model '{name}' not registered.")
        backend = config['backend']
        if backend == 'st':
            emb = STEmbedder(config['model'], device=config.get('device', self.default_device))
        elif backend == 'hf':
            emb = HFEmbedder(
                config['model'],
                device=config.get('device', self.default_device),
                pooling=config.get('pooling', 'mean')
            )
        elif backend == 'openai':
            emb = OpenAIEmbedder(config['model'], api_key=config.get('api_key'))
        elif backend == 'use':
            emb = USEEmbedder()
        else:
            raise ValueError(f"Unknown backend: {backend}")
        self._models[name] = emb
        return emb

    def list_models(self) -> List[str]:
        return list(self._registry.keys())

    def set_active_model(self, name: str) -> None:
        if name not in self._registry:
            raise ValueError(f"Model '{name}' not registered.")
        self._active = name

    def embed_text(self, text: str, **kwargs) -> np.ndarray:
        if not self._active:
            raise RuntimeError("No active model.")
        emb = self._models.get(self._active) or self._load_model(self._active)
        return emb.embed_text(text, **kwargs)

    def embed_chunks(self, chunks: List[str], **kwargs) -> np.ndarray:
        if not self._active:
            raise RuntimeError("No active model.")
        emb = self._models.get(self._active) or self._load_model(self._active)
        return emb.embed_chunks(chunks, **kwargs)


if __name__ == '__main__':
    api = EmbeddingAPI()
    print("Available models:", api.list_models())

    api.set_active_model('mpnet')
    print("Active model: mpnet")

    emb = api.embed_text("Hello from MPNet!")
    print("Single text shape:", emb.shape)

    chunks = ["First sent.", "Second sent."]
    embs = api.embed_chunks(chunks)
    print("Chunks shape:", embs.shape)
