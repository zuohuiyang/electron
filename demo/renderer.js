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

document.getElementById('btn-heap-uaf')?.addEventListener('click', () => {
  console.log('Triggering crash: heap-uaf');
  window.api.triggerCrash('heap-uaf');
});
document.getElementById('btn-heap-overflow')?.addEventListener('click', () => {
  console.log('Triggering crash: heap-overflow');
  window.api.triggerCrash('heap-overflow');
});
document.getElementById('btn-heap-underflow')?.addEventListener('click', () => {
  console.log('Triggering crash: heap-underflow');
  window.api.triggerCrash('heap-underflow');
});
document.getElementById('btn-heap-double-free')?.addEventListener('click', () => {
  console.log('Triggering crash: heap-double-free');
  window.api.triggerCrash('heap-double-free');
});