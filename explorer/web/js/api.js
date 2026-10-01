// The one call the UI makes: POST /api/solve with the existing request
// contract. The response is returned as the service sent it.

export const SOLVE_URL = '/api/solve';
export const HEALTH_URL = '/api/health';

export function buildRequest(modelText, options) {
  return { model: { format: 'mps', content: modelText }, options };
}

// -> {kind: 'response', httpStatus, body}   the service answered
//    {kind: 'transport', message}           it could not be reached / answered garbage
export async function postSolve(request, fetchImpl = globalThis.fetch) {
  let response;
  try {
    response = await fetchImpl(SOLVE_URL, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(request),
    });
  } catch {
    return { kind: 'transport', message: 'The Explorer service could not be reached. Is it running?' };
  }
  let body;
  try {
    body = await response.json();
  } catch {
    return { kind: 'transport', message: `The Explorer service answered HTTP ${response.status} without a JSON body.` };
  }
  if (!body || typeof body !== 'object' || typeof body.outcome !== 'string') {
    return { kind: 'transport', message: `The Explorer service answered HTTP ${response.status} with an unexpected body.` };
  }
  return { kind: 'response', httpStatus: response.status, body };
}

// One solve at a time: a second run() while one is in flight is refused
// rather than queued, so a double click never submits twice.
export function createRunner(fetchImpl = globalThis.fetch) {
  let busy = false;
  return {
    get busy() { return busy; },
    async run(request) {
      if (busy) return null;
      busy = true;
      try {
        return await postSolve(request, fetchImpl);
      } finally {
        busy = false;
      }
    },
  };
}

export async function checkHealth(fetchImpl = globalThis.fetch) {
  try {
    const response = await fetchImpl(HEALTH_URL);
    const body = await response.json();
    return response.ok && body.status === 'ok' ? { ok: true, contract: body.contract } : { ok: false };
  } catch {
    return { ok: false };
  }
}
