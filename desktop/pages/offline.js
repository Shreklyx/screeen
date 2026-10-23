'use strict';

const params = new URLSearchParams(location.search);
document.getElementById('url').textContent = params.get('url') || '';
document.getElementById('error').textContent = params.get('error') || '';
document.getElementById('retry').addEventListener('click', () => window.screenshareApp.retry());
document.getElementById('settings').addEventListener('click', () => window.screenshareApp.openSettings());
// try again by itself every 10 seconds
setInterval(() => window.screenshareApp.retry(), 10000);
