'use strict';

const input = document.getElementById('url');
window.screenshareApp.getServerUrl().then((url) => {
  input.value = url;
  input.select();
});
document.getElementById('form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const res = await window.screenshareApp.setServerUrl(input.value);
  if (res.ok) window.close();
  else document.getElementById('error').textContent = res.error || 'Nem sikerült menteni.';
});
document.getElementById('cancel').addEventListener('click', () => window.close());
