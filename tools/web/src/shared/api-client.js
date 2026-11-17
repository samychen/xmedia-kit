export function api(path, body) {
  return fetch(path, { method: body ? "POST" : "GET",
    headers: { "Content-Type": "application/json" },
    body: body ? JSON.stringify(body) : undefined }).then(function (r) {
      return r.json().then(function (j) {
        if (j.error) throw new Error(j.error);
        return j;
      });
    });
}
