"""Submit MSIX to Microsoft Store via Partner Center Submission API.

Usage:
  python msix/store_submit.py <msix_path>

Credentials file (JSON) at ~/.openzip_store.json:
  {
    "tenant_id":      "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
    "client_id":      "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
    "client_secret":  "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
    "application_id": "9P09P5W5DPK0"
  }

Prerequisites (one-time):
  1. Partner Center account linked to Entra ID tenant (Global admin).
  2. Azure AD app registered, granted Manager role in Partner Center.
  3. App has at least one UI-completed submission (age ratings answered).

Never edit API-created submissions in the Partner Center UI — corrupts them.
"""
import json
import os
import sys
import time
import zipfile

import requests

CRED_FILE = os.path.expanduser('~/.openzip_store.json')
TOKEN_URL = 'https://login.microsoftonline.com/{tenant}/oauth2/token'
API_BASE = 'https://manage.devcenter.microsoft.com/v1.0/my'
RESOURCE = 'https://manage.devcenter.microsoft.com'

# States we treat as "still running"
PENDING_STATES = {'CommitStarted', 'PreProcessing'}
# Terminal success states (certification started or later)
ACCEPTED_STATES = {'Certification', 'Release', 'PendingPublication',
                   'Publishing', 'Published'}


def load_credentials():
    if not os.path.isfile(CRED_FILE):
        sys.exit(f"Credentials file not found: {CRED_FILE}\n"
                 f"See module docstring for required format.")
    with open(CRED_FILE, encoding='utf-8') as f:
        c = json.load(f)
    for key in ('tenant_id', 'client_id', 'client_secret', 'application_id'):
        if not c.get(key):
            sys.exit(f"Missing '{key}' in {CRED_FILE}")
    return c


def get_token(cred):
    r = requests.post(
        TOKEN_URL.format(tenant=cred['tenant_id']),
        data={
            'grant_type': 'client_credentials',
            'client_id': cred['client_id'],
            'client_secret': cred['client_secret'],
            'resource': RESOURCE,
        },
        timeout=30,
    )
    r.raise_for_status()
    return r.json()['access_token']


def api(method, token, path, **kwargs):
    url = f"{API_BASE}{path}"
    headers = kwargs.pop('headers', {})
    headers['Authorization'] = f'Bearer {token}'
    if 'json' in kwargs or method in ('PUT', 'POST'):
        headers.setdefault('Content-Type', 'application/json')
    r = requests.request(method, url, headers=headers, timeout=60, **kwargs)
    corr = r.headers.get('MS-CorrelationId', '-')
    if not r.ok:
        sys.exit(f"API {method} {path} → {r.status_code} (corr={corr})\n{r.text}")
    return r.json() if r.text else {}, corr


def zip_msix(msix_path):
    """Wrap MSIX in a ZIP (Store API requires ZIP upload, MSIX at root)."""
    zip_path = msix_path + '.upload.zip'
    msix_name = os.path.basename(msix_path)
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.write(msix_path, arcname=msix_name)
    return zip_path, msix_name


def upload_blob(sas_url, zip_path):
    """PUT ZIP to Azure Blob via SAS URI. Single-shot block blob upload."""
    with open(zip_path, 'rb') as f:
        data = f.read()
    r = requests.put(
        sas_url,
        data=data,
        headers={
            'x-ms-blob-type': 'BlockBlob',
            'Content-Type': 'application/zip',
        },
        timeout=600,
    )
    if not r.ok:
        sys.exit(f"Blob upload failed: {r.status_code}\n{r.text}")


def submit(msix_path):
    if not os.path.isfile(msix_path):
        sys.exit(f"MSIX not found: {msix_path}")

    cred = load_credentials()
    app_id = cred['application_id']
    token = get_token(cred)

    # 1. Get app, cancel any pending submission
    print("  checking app status...", end='', flush=True)
    app, _ = api('GET', token, f'/applications/{app_id}')
    pending = app.get('pendingApplicationSubmission')
    if pending:
        print(f"\n  canceling pending submission {pending['id']}...", end='', flush=True)
        api('DELETE', token, f"/applications/{app_id}/submissions/{pending['id']}")
    print("ok")

    # 2. Create new submission (clones last published)
    print("  creating submission...", end='', flush=True)
    sub, _ = api('POST', token, f'/applications/{app_id}/submissions')
    sub_id = sub['id']
    upload_url = sub['fileUploadUrl']
    print(f"id={sub_id}")

    # 3. Update submission JSON: replace packages[]
    _, msix_name = zip_msix(msix_path)
    sub['packages'] = [{'fileName': msix_name, 'fileStatus': 'PendingUpload'}]
    print("  updating package list...", end='', flush=True)
    api('PUT', token, f'/applications/{app_id}/submissions/{sub_id}',
        data=json.dumps(sub))
    print("ok")

    # 4. Upload ZIP to Azure Blob
    zip_path = msix_path + '.upload.zip'
    size_mb = os.path.getsize(zip_path) / (1024 * 1024)
    print(f"  uploading {size_mb:.1f} MB to blob...", end='', flush=True)
    upload_blob(upload_url, zip_path)
    os.remove(zip_path)
    print("ok")

    # 5. Commit
    print("  committing...", end='', flush=True)
    api('POST', token, f'/applications/{app_id}/submissions/{sub_id}/commit')
    print("ok")

    # 6. Poll until commit/preprocessing finishes (~few minutes)
    print("  polling status (up to 10 min)...")
    deadline = time.time() + 600
    last_status = None
    while time.time() < deadline:
        status_res, corr = api(
            'GET', token, f'/applications/{app_id}/submissions/{sub_id}/status'
        )
        status = status_res.get('status', 'Unknown')
        if status != last_status:
            print(f"    [{status}] corr={corr}")
            last_status = status

        if status in ACCEPTED_STATES:
            print(f"  accepted — now in {status}. Review progress in Partner Center.")
            return True
        if status not in PENDING_STATES:
            # Failure state: CommitFailed, PreProcessingFailed, etc.
            errors = status_res.get('statusDetails', {}).get('errors', [])
            for e in errors:
                print(f"    ERROR: {e.get('code')}: {e.get('details')}")
            sys.exit(f"Submission failed: {status}")

        time.sleep(20)

    print("  still processing after 10 min — check Partner Center for final status.")
    return True


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit("Usage: python msix/store_submit.py <msix_path>")
    submit(sys.argv[1])
