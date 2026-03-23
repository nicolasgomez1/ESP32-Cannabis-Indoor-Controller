const JSVersion='V420260323_0602';
let bFirst=true;

function GetElement(n){return document.getElementById(n)}

function SetWheelSpinRange(e,min,max,step){
	e=GetElement(e);
	e.min=min;
	e.step=step;

	for(i=min;i<=max+Number.EPSILON;i=Math.round((i+step)*1000)/1000){
		let l=document.createElement('div');
		l.className='wheellabel';
		l.textContent=i;
		e.appendChild(l);
	}
}

function SetWheelSpinValue(e,v){
	e=GetElement(e);
	e.scrollTop=Math.round((v-e.min)/e.step)*15;
}

function GetWheelValue(e){return parseFloat(e.children[Math.round(e.scrollTop/15)].textContent)}

let elements=[
	{e:'lightstart',min:1,max:24,step:1},
	{e:'lightstop',min:1,max:24,step:1},
	{e:'idc',min:1,max:365,step:1},
	{e:'ifts',min:0,max:100,step:1},
	{e:'rfts',min:0,max:100,step:1},
	{e:'rfhs',min:0,max:100,step:1},
	{e:'temphys',min:0,max:100,step:1},
	{e:'humhys',min:0,max:100,step:1},
	{e:'restint',min:0,max:1440,step:1},
	{e:'restdur',min:0,max:1440,step:1},
	{e:'ifpm',min:0,max:1000,step:1},
	{e:'pumpfpm0',min:0,max:1000,step:1},
	{e:'pumpfpm1',min:0,max:1000,step:1},
	{e:'pumpfpm2',min:0,max:1000,step:1},
	{e:'mixdur',min:0,max:1330,step:1},
	{e:'lrlw',min:0,max:100,step:1},
	{e:'saint',min:1,max:1440,step:1}
];

elements.forEach(_e=>SetWheelSpinRange(_e.e,_e.min,_e.max,_e.step));

let e_profile=GetElement('profile'),e_lightstart=GetElement(elements[0].e),e_lightstop=GetElement(elements[1].e),e_fim=GetElement('fim');

let rc=['#EC6066','#6699CC','#99C794','#F9AE58'];

let vpdranges=[
	{min:0,max:.3,c:rc[0],s:'Peligro, Muy Húmedo'},
	{min:.3,max:.6,c:rc[1],s:'Propagación'},
	{min:.6,max:1.0,c:rc[2],s:'Vegetativo'},
	{min:1.0,max:1.6,c:rc[3],s:'Floración'},
	{min:1.6,max:6.27,c:rc[0],s:'Peligro, Muy Seco'}
];

let tempranges=[
	{min:0,max:18,c:rc[1]},
	{min:18,max:26,c:rc[2]},
	{min:26,max:30,c:rc[3]},
	{min:30,max:60,c:rc[0]}
];

let humranges=[
	{min:0,max:40,c:rc[0]},
	{min:40,max:50,c:rc[1]},
	{min:50,max:70,c:rc[2]},
	{min:70,max:85,c:rc[3]},
	{min:85,max:100,c:rc[0]}
];

let Chart1Labels=[
	{label:'Solución de Riego',borderColor:'#6699CC',yAxisID:'s'},
	{label:'Reductor de pH',borderColor:'#FABA18'},
	{label:'Fertilizante de Vegetativo',borderColor:'#264072'},
	{label:'Fertilizante de Floración',borderColor:'#ED6D58'}
];

function Flash(){
	let e=GetElement('updateflash');

	e.style.visibility='visible';

	setTimeout(()=>{e.style.visibility='hidden'},300);
}

function GetLightStartStop(){return[GetWheelValue(e_lightstart),GetWheelValue(e_lightstop)]}

function CalcLightDur(){
	let r=GetLightStartStop(),ch=new Date().getHours();

	GetElement('ind_meri_start').innerText=(r[0]>=12?'PM':'AM');
	GetElement('ind_meri_stop').innerText=(r[1]>=12?'PM':'AM');

	let e=GetElement('ind_ld'),h=(r[1]-r[0]+24)%24,c='#D8DEE9';

	if(h<(parseInt(e_profile.value)==1?5:4))
		c='#FF1200';

	e.style.color=c;
	e.innerText='Horas de Luz: '+h;

	GetElement('ls').setAttribute('fill',(r[0]<r[1]&&ch>=r[0]&&ch<r[1])||(r[0]>=r[1]&&(ch>=r[0]||ch<r[1]))?'#FFCF47':'#CFCFCF');
}

function SetPhoto(a,b){
	ElementsValues[0]=parseInt(a);
	ElementsValues[1]=parseInt(b);

	SetWheelSpinValue(elements[0].e,a);
	SetWheelSpinValue(elements[1].e,b);

	CalcLightDur();
}

function CalcBright(v){return`Aprox: ${(Math.round(v/MaxLightBright*100)/100*1008).toFixed(0)}ppfd`}

function SetLightBright(s){
	let e=GetElement('lb'),v=parseInt(e.value),p=Math.round(v/MaxLightBright*100);

	GetElement('ind_lp').innerText=p+'%';
	GetElement('ind_lb').innerText=p>0?CalcBright(v):'Apagado';

	if(s)
		Send(e,v);
}

function SendAction(action,...args){
	let currentUrl=new URL(window.location.href);
	currentUrl.searchParams.set('action',action);

	if(args.length%2===0){
		if(args[0]==e_lightstart.id||args[0]==e_lightstop.id){
			let r=GetLightStartStop(),min=parseInt(e_profile.value)==1?5:4;
			if((r[1]-r[0]+24)%24<min){
				alert(`No se puede definir un Fotoperiodo inferior a ${min} Horas.`);
				SetPhoto(ElementsValues[0],ElementsValues[1]);
				return;
			}else{
				ElementsValues[0]=r[0];
				ElementsValues[1]=r[1];
			}
		}

		for(let i=0;i<args.length;i+=2)
			currentUrl.searchParams.set(args[i],args[i+1]);
	}

	fetch(currentUrl).then(response=>response.text()).then(r=>{
		if(r.substring(0,6)=='RELOAD'){
			let d=r.substring(6).split(':');

			for(let i=0;i<d.length;i+=2){
				let e=d[i],v=d[i+1];

				if(e!==undefined&&v!==undefined){
					if(e=='profilechanged'){
						let cp=parseInt(e_profile.value);
						alert('Se cambió al Perfil: '+(cp==0?'Vegetativo':cp==1?'Floración':'Secado')+'.\r\nAdemás se reinició el Contador de Días de Riegos transcurridos.\r\nSe iniciará a Regar a partir de la siguiente hora.');
					}else if(e=='ichart'){
						for(let h=0;h<Chart1Labels.length;h++)
							ichart.data.datasets[h].data=[];

						let s=v.split(',');
						for(let j=0;j<s.length;j++){
							let n=s[j].split('|');

							for(let k=1;k<n.length;k++)
								ichart.data.datasets[k-1].data[j]={x:n[0],y:n[k]};
						}

						ichart.update();
					}else if(e=='lightstartstop'){
						let s=v.split(',');
						SetPhoto(s[0],s[1]);
					}else if(e=='lb'||e=='ifm'||e=='rfm'||e=='fim'){
						GetElement(e).value=v;

						if(e=='lb')
							SetLightBright();
						else if(e=='fim')
							SetFertsIncorporationMode();
						else
							SetFanMode(e);
					}else{
						if(e=='idc')
							ElementsValues[2]=parseInt(v);

						SetWheelSpinValue(e,v);
					}
				}
			}
		}else if(r.substring(0,3)=='ERR'){
			let d=r.substring(3).split(':');

			if(d[0]=='0'){
				e_profile.value=parseInt(d[1]);
				SetSelectedProfile();
				alert(d[2]);
			}
		}else if(r.substring(0,3)=='MSG'){
			alert(r.substring(3));
		}else if(r.substring(0,7)=='REFRESH'){
			let data=r.substring(7).split(':');

			CalcLightDur();

			SetEnvValues(data[0],data[1]);

			TrapezoidIndicator('reservoirlevel',data[2]);

			CalcCDC();

			let soils=data[3].split(',');

			for(let i=0;i<soils.length;i++)
				GetElement('ind_soil'+i).innerText=`Maceta ${i}: ${soils[i]}%`;

			GetElement('currenttime').innerText='Fecha: '+new Date(parseInt(data[4])*1000).toLocaleString('es-AR',{day:'2-digit',month:'2-digit',year:'numeric',hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false});

			let result='',resttime=parseInt(data[5]);

			if(resttime>0)
				result='En Reposo...<br>Tiempo Restante: '+CalcTime(resttime);

			GetElement('ind_fansrest').innerHTML=result;

			for(let i=0;i<2;i++)
				document.querySelector('.f'+i).style.animationPlayState=data[6+i]=='0'?'running':'paused';

			let idc=parseInt(data[8]);

			if(idc!=ElementsValues[2]){
				ElementsValues[2]=idc;

				SetWheelSpinValue(elements[2].e,idc);
			}

			let wattimerem=parseInt(data[9]);
			result='';

			if(wattimerem>0)
				result='Regando...<br>Tiempo Restante: '+CalcTime(wattimerem);

			GetElement('ind_irrstate').innerHTML=result;

			let v=data[10],fe=GetElement('firmver');

			if(v!=fe.innerText){
				GetElement('loaderoverlay').style.display='none';
				fe.innerText=v;
			}

			if(data[11]){
				let cd=data[11].split(',');

				for(let i=0;i<cd.length;i++){
					let v=cd[i].split('|');

					for(let j=1;j<v.length;j++){
						hchart.data.datasets[j>2?j:j-1].data[i]={x:v[0],y:parseFloat(v[j]),string:v[j]};

						if(j==2){
							let r=CalcVPD(v[1],v[2]).toFixed(2);

							hchart.data.datasets[j].data[i]={x:v[0],y:r,string:r};
						}
					}
				}

				hchart.update();

				let tablechild={},table=document.querySelector('#legend tbody');

				table.querySelectorAll('td.sl').forEach(td=>{tablechild[td.getAttribute('ddi')]=td.classList.contains('hl')});
				table.innerHTML='';

				hchart.data.datasets.forEach((ds,i)=>{
					if(i==5)
						return;

					let v=ds.data.map(point=>point.y),r=document.createElement('tr'),s=ds.symbol,min,max,avg;

					if(i==2){
						let t=hchart.data.datasets[0].data.map(point=>point.y),h=hchart.data.datasets[1].data.map(point=>point.y),vpds=[];

						for(let j=0;j<t.length;j++)
							vpds.push(CalcVPD(t[j],h[j]));

						min=Math.min(...vpds).toFixed(2);
						max=Math.max(...vpds).toFixed(2);
						avg=(vpds.reduce((s,v)=>s+v,0)/vpds.length).toFixed(2);
						min+=s+` (${GetStateOfVPD(min)})`;
						max+=s+` (${GetStateOfVPD(max)})`;
						avg+=s+` (${GetStateOfVPD(avg)})`;
					}else{
						min=Math.min(...v)+s;
						max=Math.max(...v)+s;
						avg=(v.reduce((s,v)=>s+v,0)/v.length).toFixed()+s;
					}

					r.innerHTML=`<td style=color:${ds.borderColor} class='sl ${tablechild[i]?'hl':''}'ddi=${i}>${ds.label}</td><td>${min}</td><td>${max}</td><td>${avg}</td>`;

					table.appendChild(r);
				});
			}

			if(bFirst){
				bFirst=false;

				GetElement('loaderoverlay').style.display='none';
			}

			Flash();
		}
	});
}

let sdb;
function Send(e,v){
	clearTimeout(sdb);
	sdb=setTimeout(()=>SendAction('update',e.id,v),1000);
}

function CalcCDC(){
	let date=GetElement('cb').innerText.split('/');
	GetElement('cdc').innerText=Math.floor((new Date()-new Date(parseInt(date[2],10),parseInt(date[1],10)-1,parseInt(date[0],10)))/(1000*60*60*24));
}

function SetCropBegin(){
	let e=GetElement('cb'),utc=Math.floor(Date.now()/1000);

	e.innerText=new Date(utc*1000).toLocaleDateString('es-AR',{day:'2-digit',month:'2-digit',year:'numeric'});

	CalcCDC();

	SendAction('update',e.id,utc);
}

function SetFanMode(n,s){
	let e=GetElement(n),v=parseInt(e.value);

	GetElement('ind_'+n).innerText=v==0?'OFF':v==1?'AUTO':'MANUAL';

	if(s)
		SendAction('update',e.id,v);
}

function GetStateOfVPD(v){
	if(v<=vpdranges[4].max){
		for(let i=0;i<vpdranges.length;i++){
			let r=vpdranges[i],il=i==vpdranges.length-1;

			if(v>=r.min&&(il?v<=r.max:v<r.max))
				return vpdranges[i].s;
		}
	}

	return'Fuera de rango';
}

function CalcVPD(t,h){
	t=parseFloat(t),h=parseFloat(h);

	let e=6.112*Math.exp((17.67*t)/(243.5+t));

	return (e-(h/100)*e)/10;
}

function CalcTime(s){
	let r;

	if(s>0){
		if(s<60){
			r=s+' segundos';
		}else{
			let v=Math.floor(s/60);
			r=v+(v==1?' minuto':' minutos');
			v=s%60;

			if(v>0)
				r+=` y ${v} segundos`;
		}
	}

	return r;
}

function CalcIrrigation(cc){
	let r=GetLightStartStop(),dpm=parseInt(GetElement('ifpm').value);
	let effstart=r[0]==24?0:r[0],effstop=r[1]==24?0:r[1],irrstart=(effstart+2)%24,div=parseInt(e_profile.value)==1?2:1,avai=(((effstop-2+24)-irrstart)%24)/div,ccpp=cc/avai,hours=[];

	for(let i=0;i<avai;i++){
		let h=(irrstart+i*div)%24;

		hours.push(h+(h>=12?'PM':'AM'));
	}

	return[avai,ccpp,dpm>0?(ccpp/dpm)*60:0,hours];
}

function SetSelectedProfile(s){
	let v=parseInt(e_profile.value);

	GetElement('ind_profile').innerText=v==0?'Vegetativo':v==1?'Floración':'Secado';

	if(s)
		SendAction('reload',e_profile.id,v);
}

function SetFertsIncorporationMode(s){
	let v=parseInt(e_fim.value);

	GetElement('ind_fim').innerText=v==0?'Permisivo':'Estricto';

	CalcFertsIncorporation();

	if(s)
		SendAction('update',e_fim.id,v);
}

function CalcFertsIncorporation(){
	let m=parseInt(e_fim.value),idc=GetWheelValue(GetElement(elements[2].e));
	let ids=ichart.data.datasets[0],fds=ichart.data.datasets.slice(1),hti=false,di=-1;

	for(let i=ids.data.length-1;i>=0;i--){
		let d=parseInt(ids.data[i].x);

		if(!hti&&idc>=d&&parseFloat(ids.data[i].y)>0){
			if(m==0){
				hti=true;
			}else if(m==1){
				for(let j=0;j<fds.length;j++){
					if(parseFloat(fds[j].data[i].y)>.001){
						hti=true;
						break;
					}
				}
			}
		}

		if(!hti&&idc>=d)
			break;

		if(hti&&fds.some(ds=>parseFloat(ds.data[i].y)>.001)){
			di=i;
			break;
		}
	}

	let r='<ind class=state>No se Incorporarán Fertilizantes.</ind>';

	if(di>=0){
		r='Se van a Incorporar Fertilizantes del Día: '+ids.data[di].x;

		fds.forEach(ds=>{
			let cc=parseFloat(ds.data[di].y);

			if(cc>.001)
				r+=`<br><ind style=color:${ds.borderColor}>${ds.label}</ind>: ${cc.toFixed(1)}cc`;
		});
	}

	GetElement('fti').innerHTML=r;
}

function SemiCircleGauge(e,ranges,p,...t){
	e=GetElement(e);
	e.height=70;
	e.width=160;

	p=parseFloat(p);

	let h=e.height-4,ctx=e.getContext('2d'),cx=e.width/2,apr=Math.PI/ranges.length,sa=Math.PI,a=Math.PI,pw=.06,ty=h;

	for(let i=0;i<ranges.length;i++){
		let ea=sa+apr;

		ctx.beginPath();
		ctx.arc(cx,h,h-8,sa,ea,false);
		ctx.strokeStyle=ranges[i].c;
		ctx.lineWidth=11;
		ctx.stroke();

		sa=ea;
	}

	if(p>=ranges[ranges.length-1].max){
		a=2*Math.PI;
	}else{
		for(let i=0;i<ranges.length;i++){
			let r=ranges[i];

			if(p>=r.min&&p<=r.max){
				a+=(i+(p-r.min)/(r.max-r.min))*apr;
				break;
			}
		}
	}

	ctx.beginPath();
	ctx.arc(cx,h,h-16,a-pw,a+pw);
	ctx.arc(cx,h,h,a+pw,a-pw,true);
	ctx.closePath();
	ctx.fillStyle='#2A8387';
	ctx.fill();

	ctx.fillStyle='#D8DEE9';
	ctx.textAlign='center';
	ctx.textBaseline='middle';

	if(e.id=='meter_vpd'){
		ctx.font='14px Arial';

		ty-=17;

		p=p.toFixed(2);
	}else{
		ctx.font='16px Arial';

		p=p.toFixed(1);
	}

	ctx.fillText(p+t[0],cx,ty-4);

	if(t[1])
		ctx.fillText(t[1],cx,ty+10);
}

function SetEnvValues(t,h){
	let vpd=CalcVPD(t,h);

	SemiCircleGauge('meter_temp',tempranges,t,'°C');
	SemiCircleGauge('meter_hum',humranges,h,'%');
	SemiCircleGauge('meter_vpd',vpdranges,vpd,'kPa',GetStateOfVPD(vpd));
}

function TrapezoidIndicator(e,l){
	e=GetElement(e);
	e.height=74;
	e.width=72;

	let ctx=e.getContext('2d'),ox=(e.width-60)/2,tl=[ox,1],tr=[ox+60,1],bl=[ox+10/2,69],br=[ox+110/2,69];

	ctx.beginPath();
	ctx.moveTo(tl[0]+3,tl[1]);
	ctx.lineTo(tr[0]-3,tr[1]);
	ctx.quadraticCurveTo(tr[0],tr[1],tr[0],tr[1]+3);
	ctx.lineTo(br[0],br[1]-3);
	ctx.quadraticCurveTo(br[0],br[1],br[0]-3,br[1]);
	ctx.lineTo(bl[0]+3,bl[1]);
	ctx.quadraticCurveTo(bl[0],bl[1],bl[0],bl[1]-3);
	ctx.lineTo(tl[0],tl[1]+3);
	ctx.quadraticCurveTo(tl[0],tl[1],tl[0]+3,tl[1]);
	ctx.closePath();
	ctx.lineWidth=1.5;
	ctx.strokeStyle='#6699CC';
	ctx.stroke();

	let ty=tl[1]+3,ih=bl[1]-5,fty=ty+(1-Math.max(.01,Math.min(l/100,1)))*ih,ftd=60-10*((fty-ty)/ih),fx=(e.width-ftd)/2;

	ctx.beginPath();
	ctx.moveTo(ox+10/2+3,66);
	ctx.lineTo(ox+110/2-3,66);
	ctx.lineTo(fx+ftd-3,fty);
	ctx.lineTo(fx+3,fty);
	ctx.closePath();
	ctx.fillStyle='#6699CC';
	ctx.fill();

	ctx.font='16px Arial';
	ctx.fillStyle='#D8DEE9';
	ctx.textAlign='center';
	ctx.shadowColor='#000';
	ctx.shadowBlur=4;
	ctx.fillText(l+'%',e.width/2,70/1.7);
}

Object.assign(Chart.defaults.datasets.line,{borderWidth:1.5,tension:.3});
Chart.defaults.layout.padding={left:20,right:20};
Chart.defaults.scales.linear={offset:false,display:false};
Chart.defaults.interaction={mode:'index',intersect:false};

Chart1Labels.forEach(ds=>{ds.backgroundColor=ds.borderColor});

let ichart=new Chart(GetElement('ichart'),{type:'line',data:{datasets:Chart1Labels},
	options:{
		maintainAspectRatio:false,
		plugins:{
			legend:{display:true,labels:{color:'#D8DEE9'}},
			tooltip:{
				callbacks:{
					title:item=>'Día: '+item[0].raw.x,
					label:item=>item.dataset.label+`: ${item.raw.y}cc`,
					footer:items=>{
						let d=CalcIrrigation(items[0].raw.y);

						return[
							'Total de Pulsos: '+d[0],
							`Cantidad de Riego por Pulso: ${d[1].toFixed(1)}cc (${(d[1]/items[0].raw.y*100).toFixed(1)}% de ${items[0].raw.y}cc)`,
							`Duración de cada Pulso: ${d[2].toFixed(1)} segundos`,
							'Horas de Riego: '+d[3].join(' '),
						];
					}
				}
			}
		},animation:{duration:0},
		scales:{
			y:{display:false},s:{display:false},
			x:{display:true,type:'linear',ticks:{stepSize:1,color:'#D8DEE9'},title:{color:'#D8DEE9',display:true,text:'Día'}}
		}
	},plugins:[{
		afterDatasetsDraw(chart){
			let{ctx,data}=chart;

			data.datasets.forEach((ds,i)=>{
				if(!chart.isDatasetVisible(i))
					return;

				chart.getDatasetMeta(i).data.forEach((p,index)=>{
					ctx.save();
					ctx.fillStyle=ds.borderColor;
					ctx.shadowColor='black';
					ctx.shadowOffsetX=1;
					ctx.shadowOffsetY=1;
					ctx.fillStyle='#D8DEE9';
					ctx.fillText(ds.data[index].y+'cc',p.x-10,p.y);
					ctx.restore();
				});
			});
		}
	}]
});

let stl=false;

Chart2Labels.forEach((ds,i)=>{
	ds.pointRadius=0;
	ds.tension=0;
	ds.backgroundColor=ds.borderColor;
	ds.yAxisID=i==2?'1':i==5?'2':'0';
});

let hchart=new Chart(GetElement('hchart'),{type:'line',data:{datasets:Chart2Labels},
	options:{
		maintainAspectRatio:false,
		plugins:{
			legend:{display:false},
			tooltip:{
				callbacks:{
					title:item=>{
						let date=new Date(item[0].raw.x*1000);
						return`Fecha: ${date.toLocaleDateString('es-AR',{day:'2-digit',month:'2-digit',year:'numeric'})} `+date.toLocaleTimeString('es-AR');
					},
					label:item=>item.dataset.label+`: ${item.dataset.symbol?item.raw.string+item.dataset.symbol+(item.dataset.symbol=='kPa'?` (${GetStateOfVPD(item.raw.string)})`:''):item.raw.string=='0'?'Apagada':`Encendida (${CalcBright(item.raw.string)})`}`
				}
			}
		},animation:{duration:0},scales:{x:{type:'linear',ticks:{stepSize:1}}}
	},plugins:[{
		afterDatasetsDraw(chart){
			if(!stl)
				return;

			let{ctx,data}=chart;

			data.datasets.forEach((ds,i)=>{
				if(ds.hidden||i==5)
					return;

				chart.getDatasetMeta(i).data.forEach((p,index)=>{
					ctx.save();
					ctx.fillStyle=ds.borderColor;
					ctx.shadowColor='black';
					ctx.shadowOffsetX=1;
					ctx.shadowOffsetY=1;
					ctx.fillText(ds.data[index].y+ds.symbol,p.x-10,p.y-3);
					ctx.restore();
				});
			});
		}
	}]
});

elements.forEach((_e,i)=>{
	let e=GetElement(_e.e);

	e.addEventListener('wheel',ev=>{
		ev.preventDefault();
		e.scrollBy({top:Math.sign(ev.deltaY)*15});

		let r=GetWheelValue(e);

		if(i<3){
			if(i<2)
				CalcLightDur();
			else
				CalcFertsIncorporation();
		}

		Send(e,r);
	});

	let tsY=0;

	e.addEventListener('touchstart',ev=>{
		tsY=ev.touches[0].clientY;

		ev.preventDefault();
	},{passive:false});

	e.addEventListener('touchmove',ev=>{
		ev.preventDefault();

		let nSteps=Math.round((tsY-ev.touches[0].clientY)/15);
		if(nSteps==0)
			return;

		e.scrollBy({top:nSteps*15});

		tsY=ev.touches[0].clientY;

		let r=GetWheelValue(e);

		if(i<3){
			if(i<2)
				CalcLightDur();
			else
				CalcFertsIncorporation();
		}

		Send(e,r);
	},{passive:false});
});

ichart.canvas.addEventListener('pointerup',e=>{
	if(e.button>0)
		return;

	let r=ichart.canvas.getBoundingClientRect();

	if(e.clientY-r.top<25)
		return;

	let d=ichart.getElementsAtEventForMode(e,'nearest',{intersect:true},true);

	let Apply=()=>{
		SendAction('update','ichart',ichart.data.datasets[0].data.map((_,i)=>ichart.data.datasets[0].data[i].x+'|'+ichart.data.datasets.map(ds=>ds.data[i].y).join('|')).join(','));
	};

	if(d.length){
		if(confirm('¿Seguro que queres eliminar este Riego?')){
			let x=ichart.data.datasets[d[0].datasetIndex].data[d[0].index].x;

			for(let i=0;i<Chart1Labels.length;i++)
				ichart.data.datasets[i].data=ichart.data.datasets[i].data.filter(p=>p.x!=x);

			ichart.update();

			Apply();
		}
	}else{
		let d=prompt('Día',Math.round(ichart.scales.x.getValueForPixel(e.clientX-r.left)));

		if(d!==null&&!isNaN(d)&&d>0){
			let c=false,td=[];

			for(let i=0;i<Chart1Labels.length;i++){
				let cc=prompt(`Cantidad de CC de ${Chart1Labels[i].label} a ${i==0?'Regar':'Incorporar'}`,Math.round(ichart.scales.y.getValueForPixel(e.clientY-r.top)));

				if(cc!==null&&!isNaN(cc)){
					td.push({dsi:i,p:{x:parseInt(d),y:parseFloat(cc)}});

					if(i+1==Chart1Labels.length)
						c=true;
				}else{
					break;
				}
			}

			if(c){
				if(confirm('¿Querés aplicar los cambios?'))
				{
					for(let t of td){
						ichart.data.datasets[t.dsi].data.push(t.p);
						ichart.data.datasets[t.dsi].data.sort((a,b)=>a.x-b.x);
					}

					ichart.update();

					Apply();
				}
			}
		}
	}
});

document.querySelector('#legend').addEventListener('click',e=>{
	if(e.target&&e.target.matches('td.sl')){
		let ds=hchart.data.datasets[e.target.getAttribute('ddi')];

		ds.hidden=!ds.hidden;

		hchart.update();

		e.target.classList.toggle('hl',ds.hidden);
	}
});

GetElement('tt').addEventListener('click',e=>{
	stl=!stl;
	e.textContent=stl?'Ocultar Valores':'Mostrar Valores';
	hchart.update();
});

GetElement('otaform').addEventListener('submit',ev=>{
	ev.preventDefault();

	let f=GetElement('otafile').files[0];
	if(!f)
		return;

	GetElement('loaderoverlay').style.display='flex';

	let fd=new FormData();
	fd.append('file',f);

	fetch('/ota',{method:'POST',body:fd}).catch(()=>{});
});

GetElement('softwareform').addEventListener('submit',async ev=>{
	ev.preventDefault();
	let f=Array.from(GetElement('softwarefile').files);
	if(!f.length)
		return;

	await fetch('/upload-clean',{method:'POST'});

	for(let file of f){
		let fd=new FormData();
		fd.append('file',file);

		let r=await fetch('/upload',{method:'POST',body:fd,headers:{'File-Size':file.size}});
		if(!r.ok){
			alert('Error al subir: '+file.name);
			await fetch('/upload-clean',{method:'POST'});
			return;
		}
	}

	let r=await fetch('/upload-commit',{method:'POST'});
	alert(await r.text());

	location.reload();
});

window.onload=function(){
	SetSelectedProfile(true);
	elements.forEach((_e,i)=>{SetWheelSpinValue(_e.e,ElementsValues[i])});
	CalcLightDur();
	SetLightBright();
	['ifm','rfm'].forEach(e=>SetFanMode(e));
	SetFertsIncorporationMode();
	GetElement('htmlver').innerText=HTMLVersion;
	GetElement('cssver').innerText=getComputedStyle(document.documentElement).getPropertyValue('--CSSVersion');
	GetElement('jsver').innerText=JSVersion;
	SendAction('refresh');
	setInterval(()=>{SendAction('refresh')},3000);
};