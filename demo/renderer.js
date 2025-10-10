document.getElementById('uaf').addEventListener('click', () => {
  console.log('Triggering crash: uaf');
  window.api.triggerCrash('uaf');
});

document.getElementById('default').addEventListener('click', () => {
  console.log('Triggering crash: default');
  window.api.triggerCrash('default');
});

document.getElementById('overflow').addEventListener('click', () => {
  console.log('Triggering crash: overflow');
  window.api.triggerCrash('overflow');
});

document.getElementById('underflow').addEventListener('click', () => {
  console.log('Triggering crash: underflow');
  window.api.triggerCrash('underflow');
});

document.getElementById('noarg').addEventListener('click', () => {
  console.log('Triggering crash: noarg');
  window.api.triggerCrash(undefined);
});

