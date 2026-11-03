import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync,mkdirSync,writeFileSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {DefaultResourceLoader} from '../runtime/subagent-sdk.mjs';

test('community children retain the parent Skill selection; disabling its Widget removes all Skills', async()=>{
  const root=mkdtempSync(join(tmpdir(),'umeko-child-skills-'));
  const key=Symbol.for('umeko:skill-policy-host'), old=globalThis[key];
  try {
    for(const name of ['kept','disabled']) {
      const folder=join(root,'skills',name);mkdirSync(folder,{recursive:true});
      writeFileSync(join(folder,'SKILL.md'),`---\nname: ${name}\ndescription: Local fixture.\n---\nLocal fixture.`);
    }
    globalThis[key]={enabled:()=>true,options:{skillsOverride:result=>({...result,skills:result.skills.filter(s=>s.name==='kept')})}};
    const options={cwd:root,agentDir:root,noExtensions:true,noContextFiles:true,noPromptTemplates:true,noThemes:true};
    const child=new DefaultResourceLoader(options);
    await child.reload();assert.deepEqual(child.getSkills().skills.map(s=>s.name),['kept']);
    globalThis[key].enabled=()=>false;
    const disabled=new DefaultResourceLoader(options);
    await disabled.reload();assert.deepEqual(disabled.getSkills().skills,[]);
  } finally {if(old===undefined)delete globalThis[key];else globalThis[key]=old;rmSync(root,{recursive:true,force:true});}
});
